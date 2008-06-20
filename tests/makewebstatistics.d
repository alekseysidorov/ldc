// Based on DSTRESS code by Thomas Kühne

module findregressions;

private import std.string;
private import std.conv;
private import std.stdio;
private import std.stream;
private import std.file;
private import std.c.stdlib;
private import std.date;
private import std.path;


enum Result{
	UNTESTED	= 0,
	PASS		= 1 << 2,
	XFAIL		= 2 << 2,
	XPASS		= 3 << 2,
	FAIL		= 4 << 2,
	ERROR		= 5 << 2,
	BASE_MASK	= 7 << 2,

	EXT_MASK	= 3,
	BAD_MSG		= 1,
	BAD_GDB		= 2,
	
	MAX		= BAD_GDB + BASE_MASK
}

char[] toString(Result r){
	switch(r & Result.BASE_MASK){
		case Result.PASS: return "PASS";
		case Result.XPASS: return "XPASS";
		case Result.FAIL: return "FAIL";
		case Result.XFAIL: return "XFAIL";
		case Result.ERROR: return "ERROR";
		case Result.UNTESTED: return "UNTESTED";
		default:
			break;
	}
	throw new Exception(format("unhandled Result value %s", cast(int)r));
}

char[] dateString(){
	static char[] date;
	if(date is null){
		auto time = getUTCtime();
		auto year = YearFromTime(time);
		auto month = MonthFromTime(time);
		auto day = DateFromTime(time);
		date = format("%d-%02d-%02d", year, month+1, day); 
	}
	return date;
}

char[][] unique(char[][] a){
	char[][] b = a.sort;
	char[][] back;

	back ~= b[0];

	size_t ii=0;
	for(size_t i=0; i<b.length; i++){
		if(back[ii]!=b[i]){
			back~=b[i];
			ii++;
		}
	}

	return back;	
}

private{
	version(Windows){
		import std.c.windows.windows;
		extern(Windows) BOOL GetFileTime(HANDLE hFile, LPFILETIME lpCreationTime, LPFILETIME lpLastAccessTime, LPFILETIME lpLastWriteTime);
	}else version(linux){
		import std.c.linux.linux;
		version = Unix;
	}else version(Unix){
		import std.c.unix.unix;
	}else{
		static assert(0);
	}

	alias ulong FStime;

	FStime getFStime(char[] fileName){
		version(Windows){
			HANDLE h;
		
			if (useWfuncs){
				wchar* namez = std.utf.toUTF16z(fileName);
				h = CreateFileW(namez,GENERIC_WRITE,0,null,OPEN_ALWAYS,
					FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,cast(HANDLE)null);
			}else{
				char* namez = toMBSz(fileName);
				h = CreateFileA(namez,GENERIC_WRITE,0,null,OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,cast(HANDLE)null);
			}

			if (h == INVALID_HANDLE_VALUE)
				goto err;

			FILETIME creationTime;
			FILETIME accessTime;
			FILETIME writeTime;
		
			BOOL b = GetFileTime(h, &creationTime, &accessTime, &writeTime);
			if(b==1){
				long modA = writeTime.dwLowDateTime;
				long modB = writeTime.dwHighDateTime;
				return modA  | (modB << (writeTime.dwHighDateTime.sizeof*8));
			}

err:
			CloseHandle(h);
			throw new Exception("failed to query file modification : "~fileName);
		}else version(Unix){
			char* namez = toStringz(fileName);
			struct_stat statbuf;
		
			if(stat(namez, &statbuf)){
				throw new FileException(fileName, getErrno());
			}

			return statbuf.st_mtime;
		}else{
			static assert(0);
		}
	}
}

char[] cleanFileName(char[] file){
	char[] back;
	bool hadSep;

	foreach(char c; file){
		if(c == '/' || c == '\\'){
			if(!hadSep){
				back ~= '/';
				hadSep = true;
			}
		}else{
			back ~= c;
			hadSep = false;
		}
	}

	size_t start = 0;
	while(back[start] <= ' ' && start < back.length){
		start++;
	}

	size_t end = back.length-1;
	while(back[end] <= ' ' && end >= start){
		end--;
	}

	back = back[start .. end+1];

	return back;
}

class Test{
	char[] name;
	char[] file;
	Result r;

	this(char[] file){
		this.file = file;

		int start = rfind(file, "/");
		if(start<0){
			start = 0;
		}else{
			start += 1;
		}
		
		int end = rfind(file, ".");
		if(end < start){
			end = file.length;
		}

		name = file[start .. end];
	}
}


class Log{
	Test[char[]] tests;

	char[] id;

	int[Result] counts;

	this(char[] id, char[] file){
		this.id = id;
		counts = [
			Result.PASS: 0,
			Result.FAIL: 0,
			Result.XPASS: 0,
			Result.XFAIL: 0,
			Result.ERROR: 0 ];

		writefln("parsing: %s", file);
		FStime logTime  = getFStime(file);
		Stream source = new BufferedFile(file, FileMode.In);
		while(!source.eof()){
			add(source.readLine());
		}		
		dropBogusResults(logTime, "dstress");
	}


	void dropBogusResults(FStime recordTime, char[] testRoot){
		uint totalCount = tests.length; 
		
		char[][] sourcesTests = tests.keys;
		foreach(char[] source; sourcesTests){
			if(find(source, "complex/") < 0){
				try{
					FStime caseTime = getFStime(testRoot~std.path.sep~source);
					if(caseTime > recordTime){
						debug(drop) fwritefln(stderr, "dropped: %s", source);
						counts[tests[source].r & Result.BASE_MASK]--;
						tests.remove(source);
					}
				}catch(Exception e){
					debug(drop) fwritefln(stderr, "dropped: %s", source);
					counts[tests[source].r & Result.BASE_MASK]--;
					tests.remove(source);
				}
			}
			// asm-filter
			int i = find(source, "asm_p");
			if(i >= 0){
				counts[tests[source].r & Result.BASE_MASK]--;
				tests.remove(source);
			}
		}
		tests.rehash;
		
		writefln("dropped %s outdated tests (%s remaining)", totalCount - tests.length, tests.length); 
	}

	
	bool add(char[] line){
		const char[] SUB = "Torture-Sub-";
		const char[] TORTURE = "Torture:";

		line = strip(line);
		int id = -1;
		Result r = Result.UNTESTED;

		if(line.length > SUB.length && line[0 .. SUB.length] == SUB){
			line = line[SUB.length .. $];
			id = 0;
			while(line[id] >= '0' && line[id] <= '9'){
				id++;
			}
			int start = id;
			id = std.conv.toUint(line[0 .. id]);

			while(line[start] != '-'){
				start++;
			}
			line = line[start+1 .. $];
		}

		char[][] token = split(line);
		if(token.length < 2){
			return false;
		}
		char[] file = strip(token[1]);

		switch(token[0]){
			case "PASS:":
				r = Result.PASS; break;
			case "FAIL:":
				r = Result.FAIL; break;
			case "XPASS:":
				r = Result.XPASS; break;
			case "XFAIL:":
				r = Result.XFAIL; break;
			case "ERROR:":
				r = Result.ERROR; break;
			default:{
				if(token[0] == TORTURE){
					throw new Exception("not yet handled: "~line);
				}else if(id > -1){
					throw new Exception(format("bug in SUB line: (%s) %s", id, line));
				}
			}
		}

		if(r != Result.UNTESTED){
			if(std.string.find(line, "bad error message") > -1){
				r |= Result.BAD_MSG;	
			}
			if(std.string.find(line, "bad debugger message") > -1){
				r |= Result.BAD_MSG;	
			}
			
			file = cleanFileName(file);
			
			if(id >= 0){
				// update sub
				id--;
		
				Test* test = file in tests;

				if(test is null){
					Test t = new Test(file);
					tests[file] = t;
					t.r = r;
					counts[r & Result.BASE_MASK]++;
				}else{
					if(test.r != Result.UNTESTED){
						test.r = Result.UNTESTED;
					}
					test.r = r;
				}
			}
			return true;
		}
		return false;
	}
}


char[] basedir = "web";
bool regenerate = false;

int main(char[][] args){

	if(args.length < 3 || (args[1] == "--regenerate" && args.length < 4)){
		fwritefln(stderr, "%s [--regenerate] <reference-log> <log> <log> ...", args[0]);
		fwritefln(stderr, "bash example: %s reference/dmd-something $(ls reference/llvmdc*)", args[0]);
		return 1;
	}

	char[] reference;
	char[][] files;
	if(args[1] == "--regenerate") {
		regenerate = true;
		reference = args[2];
		files = args[3..$] ~ reference;
	} else {
		reference = args[1];
		files = args[2..$] ~ reference;
	}

	// make sure base path exists
	if(std.file.exists(basedir) && !std.file.isdir(basedir))
		throw new Exception(basedir ~ " is not a directory!");
	else if(!std.file.exists(basedir))
		std.file.mkdir(basedir);
	
	
	Log[char[]] logs;

	// emit per-log data
	foreach(char[] file; files) 
		generateLogStatistics(file, logs);
	
	// differences between logs
	foreach(int i, char[] file; files[1 .. $])
		generateChangeStatistics(files[1+i], files[1+i-1], logs);

	// differences between reference and logs
	foreach(char[] file; files[0..$-1])
		generateChangeStatistics(file, reference, logs);

	// collect all the stats.base files into a large table
	BufferedFile index = new BufferedFile(std.path.join(basedir, "index.html"), FileMode.OutNew);
	scope(exit) index.close();
	index.writefln(`
		<html><body>
		<table style="border-collapse:collapse; text-align:center;">
		<colgroup>
			<col style="border-right: medium solid black;">
			<col style="background-color: #AAFFAA;">
			<col style="background-color: #AAFFAA; border-right: thin solid black;">
			<col style="background-color: #FFAAAA;">
			<col style="background-color: #FFAAAA;">
			<col style="background-color: #FFAAAA;">
			<col style="border-left: medium solid black;">
		</colgroup>
		<tr style="border-bottom: medium solid black;">
			<th>name</th>
			<th style="padding-left:1em;padding-right:1em;">PASS</th>
			<th style="padding-left:1em;padding-right:1em;">XFAIL</th>
			<th style="padding-left:1em;padding-right:1em;">FAIL</th>
			<th style="padding-left:1em;padding-right:1em;">XPASS</th>
			<th style="padding-left:1em;padding-right:1em;">ERROR</th>
			<th style="padding-left:1em;padding-right:1em;">comparison to ` ~ std.path.getBaseName(reference) ~ `</th>
		</tr>
	`);

	for(int i = files.length - 1; i >= 0; --i) {
		auto file = files[i];
		index.writefln(`<tr>`);
		char[] id = std.path.getBaseName(file);
		char[] statsname = std.path.join(std.path.join(basedir, id), "stats.base");
		index.writef(cast(char[])std.file.read(statsname));

		if(i != files.length - 1) {
			index.writefln(`<td>`);
			char[] refid = std.path.getBaseName(reference);
			statsname = std.path.join(std.path.join(basedir, refid ~ "-to-" ~ id), "stats.base");
			index.writef(cast(char[])std.file.read(statsname));
			index.writefln(`</td></tr>`);
		} else {
			index.writefln(`<td></td></tr>`);
		}

		if(i == 0) {
			continue;
		}

		index.writefln(`<tr><td></td>`);
		index.writefln(`<td style="background-color:white;" colspan="5">`);
		char[] newid = std.path.getBaseName(files[i-1]);
		statsname = std.path.join(std.path.join(basedir, newid ~ "-to-" ~ id), "stats.base");
		index.writef(cast(char[])std.file.read(statsname));
		index.writefln(`</td><td></td></tr>`);
	}

	index.writefln(`</table></body></html>`);
	
	return 0;
}

void generateLogStatistics(char[] file, ref Log[char[]] logs)
{
	char[] id = std.path.getBaseName(file);
	char[] dirname = std.path.join(basedir, id);

	if(std.file.exists(dirname)) {
		if(std.file.isdir(dirname)) {
			if(!regenerate) {
				writefln("Directory ", dirname, " already exists, skipping...");
				return;
			}
		}
		else
			throw new Exception(dirname ~ " is not a directory!");
	}
	else
		std.file.mkdir(dirname);

	// parse etc.
	Log log = new Log(id, file);
	logs[id] = log;

	// write status
	{
		BufferedFile makeFile(char[] name) {
			return new BufferedFile(std.path.join(dirname, name), FileMode.OutNew);
		}
		BufferedFile[Result] resultsfile = [
			Result.PASS: makeFile("pass.html"),
			Result.FAIL: makeFile("fail.html"),
			Result.XPASS: makeFile("xpass.html"),
			Result.XFAIL: makeFile("xfail.html"),
			Result.ERROR: makeFile("error.html") ];
	
		scope(exit) {
			foreach(file; resultsfile)
				file.close();
		}
	
		foreach(file; resultsfile)
			file.writefln(`<html><body>`);
	
		foreach(tkey; log.tests.keys.sort) {
			auto test = log.tests[tkey];
			auto result = test.r & Result.BASE_MASK;
			resultsfile[result].writefln(test.name, " in ", test.file, "<br>");
		}
	
		foreach(file; resultsfile)
			file.writefln(`</body></html>`);
	}

	BufferedFile stats = new BufferedFile(std.path.join(dirname, "stats.base"), FileMode.OutNew);
	scope(exit) stats.close();
	stats.writefln(`<td style="padding-right:1em; text-align:left;">`, id, `</td>`);
	stats.writefln(`<td><a href="`, std.path.join(log.id, "pass.html"), `">`, log.counts[Result.PASS], `</a></td>`);
	stats.writefln(`<td><a href="`, std.path.join(log.id, "xfail.html"), `">`, log.counts[Result.XFAIL], `</a></td>`);
	stats.writefln(`<td><a href="`, std.path.join(log.id, "fail.html"), `">`, log.counts[Result.FAIL], `</a></td>`);
	stats.writefln(`<td><a href="`, std.path.join(log.id, "xpass.html"), `">`, log.counts[Result.XPASS], `</a></td>`);
	stats.writefln(`<td><a href="`, std.path.join(log.id, "error.html"), `">`, log.counts[Result.ERROR], `</a></td>`);
}

void generateChangeStatistics(char[] file1, char[] file2, ref Log[char[]] logs)
{
	char[] newid = std.path.getBaseName(file1);
	char[] oldid = std.path.getBaseName(file2);

	char[] dirname = std.path.join(basedir, oldid ~ "-to-" ~ newid);

	if(std.file.exists(dirname)) {
		if(std.file.isdir(dirname)) {
			if(!regenerate) {
				writefln("Directory ", dirname, " already exists, skipping...");
				return;
			}
		}
		else
			throw new Exception(dirname ~ " is not a directory!");
	}
	else
		std.file.mkdir(dirname);

	// parse etc.
	Log newLog, oldLog;
	Log getOrParse(char[] id, char[] file) {		
		if(id in logs)
			return logs[id];
		else {
			Log tmp = new Log(id, file);
			logs[id] = tmp;
			return tmp;
		}
	}
	newLog = getOrParse(newid, file1);
	oldLog = getOrParse(oldid, file2);

	int nRegressions, nImprovements, nChanges;

	{
		auto regressionsFile = new BufferedFile(std.path.join(dirname, "regressions.html"), FileMode.OutNew);
		scope(exit) regressionsFile.close();
		regressionsFile.writefln(`<html><body>`);

		auto improvementsFile = new BufferedFile(std.path.join(dirname, "improvements.html"), FileMode.OutNew);
		scope(exit) improvementsFile.close();
		improvementsFile.writefln(`<html><body>`);

		auto changesFile = new BufferedFile(std.path.join(dirname, "changes.html"), FileMode.OutNew);
		scope(exit) changesFile.close();
		changesFile.writefln(`<html><body>`);

		BufferedFile targetFile;
	
		foreach(Test t; newLog.tests.values){
			Test* oldT = t.file in oldLog.tests;
	
			if(oldT !is null){
				if(oldT.r == t.r)
					continue;
				else if(oldT.r < t.r && oldT.r && oldT.r <= Result.XFAIL){
					targetFile = regressionsFile;
					nRegressions++;
				}
				else if(t.r < oldT.r && t.r && t.r <= Result.XFAIL){
					targetFile = improvementsFile;
					nImprovements++;
				}
				else {
					targetFile = changesFile;
					nChanges++;
				}
				targetFile.writefln(toString(oldT.r), " -> ", toString(t.r), " : ", t.name, " in ", t.file, "<br>");
			}
		}

		regressionsFile.writefln(`</body></html>`);
		improvementsFile.writefln(`</body></html>`);
		changesFile.writefln(`</body></html>`);
	}

	BufferedFile stats = new BufferedFile(std.path.join(dirname, "stats.base"), FileMode.OutNew);
	scope(exit) stats.close();
	auto dir = oldid ~ "-to-" ~ newid;
	stats.writefln(`<a href="`, std.path.join(dir, "improvements.html"), `">Improvements: `, nImprovements, `</a>, `);
	stats.writefln(`<a href="`, std.path.join(dir, "regressions.html"), `">Regressions: `, nRegressions, `</a>, `);
	stats.writefln(`<a href="`, std.path.join(dir, "changes.html"), `">Changes: `, nChanges, `</a>`);
}