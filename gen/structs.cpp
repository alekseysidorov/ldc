#include <algorithm>

#include "gen/llvm.h"

#include "mtype.h"
#include "aggregate.h"
#include "init.h"
#include "declaration.h"

#include "gen/irstate.h"
#include "gen/tollvm.h"
#include "gen/arrays.h"
#include "gen/logger.h"
#include "gen/structs.h"

//////////////////////////////////////////////////////////////////////////////////////////

const llvm::Type* DtoStructType(Type* t)
{
    assert(0);
    std::vector<const llvm::Type*> types;
    return llvm::StructType::get(types);
}

//////////////////////////////////////////////////////////////////////////////////////////

llvm::Value* DtoStructZeroInit(llvm::Value* v)
{
    assert(gIR);
    uint64_t n = gTargetData->getTypeSize(v->getType()->getContainedType(0));
    //llvm::Type* sarrty = llvm::PointerType::get(llvm::ArrayType::get(llvm::Type::Int8Ty, n));
    llvm::Type* sarrty = llvm::PointerType::get(llvm::Type::Int8Ty);

    llvm::Value* sarr = new llvm::BitCastInst(v,sarrty,"tmp",gIR->scopebb());

    llvm::Function* fn = LLVM_DeclareMemSet32();
    std::vector<llvm::Value*> llargs;
    llargs.resize(4);
    llargs[0] = sarr;
    llargs[1] = llvm::ConstantInt::get(llvm::Type::Int8Ty, 0, false);
    llargs[2] = llvm::ConstantInt::get(llvm::Type::Int32Ty, n, false);
    llargs[3] = llvm::ConstantInt::get(llvm::Type::Int32Ty, 0, false);

    llvm::Value* ret = new llvm::CallInst(fn, llargs.begin(), llargs.end(), "", gIR->scopebb());

    return ret;
}

//////////////////////////////////////////////////////////////////////////////////////////

llvm::Value* DtoStructCopy(llvm::Value* dst, llvm::Value* src)
{
    Logger::cout() << "dst = " << *dst << " src = " << *src << '\n';
    assert(dst->getType() == src->getType());
    assert(gIR);

    uint64_t n = gTargetData->getTypeSize(dst->getType()->getContainedType(0));
    //llvm::Type* sarrty = llvm::PointerType::get(llvm::ArrayType::get(llvm::Type::Int8Ty, n));
    llvm::Type* arrty = llvm::PointerType::get(llvm::Type::Int8Ty);

    llvm::Value* dstarr = new llvm::BitCastInst(dst,arrty,"tmp",gIR->scopebb());
    llvm::Value* srcarr = new llvm::BitCastInst(src,arrty,"tmp",gIR->scopebb());

    llvm::Function* fn = LLVM_DeclareMemCpy32();
    std::vector<llvm::Value*> llargs;
    llargs.resize(4);
    llargs[0] = dstarr;
    llargs[1] = srcarr;
    llargs[2] = llvm::ConstantInt::get(llvm::Type::Int32Ty, n, false);
    llargs[3] = llvm::ConstantInt::get(llvm::Type::Int32Ty, 0, false);

    return new llvm::CallInst(fn, llargs.begin(), llargs.end(), "", gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////
llvm::Constant* DtoConstStructInitializer(StructInitializer* si)
{
    Logger::println("DtoConstStructInitializer: %s", si->toChars());
    LOG_SCOPE;

    const llvm::StructType* structtype = isaStruct(si->ad->llvmType);
    Logger::cout() << "llvm struct type: " << *structtype << '\n';

    assert(si->value.dim == si->vars.dim);

    std::vector<DUnionIdx> inits;
    for (int i = 0; i < si->value.dim; ++i)
    {
        Initializer* ini = (Initializer*)si->value.data[i];
        assert(ini);
        VarDeclaration* vd = (VarDeclaration*)si->vars.data[i];
        assert(vd);
        llvm::Constant* v = DtoConstInitializer(vd->type, ini);
        inits.push_back(DUnionIdx(vd->llvmFieldIndex, vd->llvmFieldIndexOffset, v));
    }

    return si->ad->llvmUnion->getConst(inits);
}

//////////////////////////////////////////////////////////////////////////////////////////

llvm::Value* DtoIndexStruct(llvm::Value* ptr, StructDeclaration* sd, Type* t, unsigned os, std::vector<unsigned>& idxs)
{
    Logger::println("checking for offset %u type %s:", os, t->toChars());
    LOG_SCOPE;

    if (idxs.empty())
        idxs.push_back(0);

    const llvm::Type* llt = llvm::PointerType::get(DtoType(t));
    const llvm::Type* st = llvm::PointerType::get(DtoType(sd->type));
    if (ptr->getType() != st) {
        assert(sd->llvmHasUnions);
        ptr = gIR->ir->CreateBitCast(ptr, st, "tmp");
    }

    for (unsigned i=0; i<sd->fields.dim; ++i) {
        VarDeclaration* vd = (VarDeclaration*)sd->fields.data[i];
        Type* vdtype = DtoDType(vd->type);
        Logger::println("found %u type %s", vd->offset, vdtype->toChars());
        assert(vd->llvmFieldIndex >= 0);
        if (os == vd->offset && vdtype == t) {
            idxs.push_back(vd->llvmFieldIndex);
            ptr = DtoGEP(ptr, idxs, "tmp");
            if (ptr->getType() != llt)
                ptr = gIR->ir->CreateBitCast(ptr, llt, "tmp");
            if (vd->llvmFieldIndexOffset)
                ptr = new llvm::GetElementPtrInst(ptr, DtoConstUint(vd->llvmFieldIndexOffset), "tmp", gIR->scopebb());
            return ptr;
        }
        else if (vdtype->ty == Tstruct && (vd->offset + vdtype->size()) > os) {
            TypeStruct* ts = (TypeStruct*)vdtype;
            StructDeclaration* ssd = ts->sym;
            idxs.push_back(vd->llvmFieldIndex);
            if (vd->llvmFieldIndexOffset) {
                Logger::println("has union field offset");
                ptr = DtoGEP(ptr, idxs, "tmp");
                if (ptr->getType() != llt)
                    ptr = gIR->ir->CreateBitCast(ptr, llt, "tmp");
                ptr = new llvm::GetElementPtrInst(ptr, DtoConstUint(vd->llvmFieldIndexOffset), "tmp", gIR->scopebb());
                std::vector<unsigned> tmp;
                return DtoIndexStruct(ptr, ssd, t, os-vd->offset, tmp);
            }
            else {
                const llvm::Type* sty = llvm::PointerType::get(DtoType(vd->type));
                if (ptr->getType() != sty) {
                    ptr = gIR->ir->CreateBitCast(ptr, sty, "tmp");
                    std::vector<unsigned> tmp;
                    return DtoIndexStruct(ptr, ssd, t, os-vd->offset, tmp);
                }
                else {
                    return DtoIndexStruct(ptr, ssd, t, os-vd->offset, idxs);
                }
            }
        }
    }

    size_t llt_sz = gTargetData->getTypeSize(llt->getContainedType(0));
    assert(os % llt_sz == 0);
    ptr = gIR->ir->CreateBitCast(ptr, llt, "tmp");
    return new llvm::GetElementPtrInst(ptr, DtoConstUint(os / llt_sz), "tmp", gIR->scopebb());
}
//////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////   D UNION HELPER CLASS   ////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////

DUnion::DUnion()
{
    DUnionField* f = NULL;
    IRStruct& topstruct = gIR->topstruct();
    bool unions = false;
    for (IRStruct::OffsetMap::iterator i=topstruct.offsets.begin(); i!=topstruct.offsets.end(); ++i)
    {
        unsigned o = i->first;
        IRStruct::Offset* so = &i->second;
        const llvm::Type* ft = so->init->getType();
        size_t sz = gTargetData->getTypeSize(ft);
        if (f == NULL) { // new field
            fields.push_back(DUnionField());
            f = &fields.back();
            f->size = sz;
            f->offset = o;
            f->init = so->init;
            f->initsize = sz; 
            f->types.push_back(ft);
        }
        else if (o == f->offset) { // same offset
            if (sz > f->size)
                f->size = sz;
            f->types.push_back(ft);
            unions = true;
        }
        else if (o < f->offset+f->size) {
            assert((o+sz) <= (f->offset+f->size));
            unions = true;
        }
        else {
            fields.push_back(DUnionField());
            f = &fields.back();
            f->size = sz;
            f->offset = o;
            f->init = so->init;
            f->initsize = sz;
            f->types.push_back(ft);
        }
    }

    {
        LOG_SCOPE;
        Logger::println("******** DUnion BEGIN");
        size_t n = fields.size();
        for (size_t i=0; i<n; ++i) {
            Logger::cout()<<"field #"<<i<<" offset: "<<fields[i].offset<<" size: "<<fields[i].size<<'('<<fields[i].initsize<<")\n";
            LOG_SCOPE;
            size_t nt = fields[i].types.size();
            for (size_t j=0; j<nt; ++j) {
                Logger::cout()<<*fields[i].types[j]<<'\n';
            }
        }
        Logger::println("******** DUnion END");
    }
}

static void push_nulls(size_t nbytes, std::vector<llvm::Constant*>& out)
{
    assert(nbytes > 0);
    std::vector<llvm::Constant*> i(nbytes, llvm::ConstantInt::get(llvm::Type::Int8Ty, 0, false));
    out.push_back(llvm::ConstantArray::get(llvm::ArrayType::get(llvm::Type::Int8Ty, nbytes), i));
}

llvm::Constant* DUnion::getConst(std::vector<DUnionIdx>& in)
{
    std::sort(in.begin(), in.end());
    std::vector<llvm::Constant*> out;

    size_t nin = in.size();
    size_t nfields = fields.size();

    size_t fi = 0;
    size_t last = 0;
    size_t ii = 0;
    size_t os = 0;

    for(;;)
    {
        if (fi == nfields) break;

        bool nextSame = (ii+1 < nin) && (in[ii+1].idx == fi);

        if (ii < nin && fi == in[ii].idx)
        {
            size_t s = gTargetData->getTypeSize(in[ii].c->getType());
            if (in[ii].idx == last)
            {
                size_t nos = in[ii].idxos * s;
                if (nos && nos-os) {
                    assert(nos >= os);
                    push_nulls(nos-os, out);
                }
                os = nos + s;
            }
            else
            {
                os = s;
            }
            out.push_back(in[ii].c);
            ii++;
            if (!nextSame)
            {
                if (os < fields[fi].size)
                    push_nulls(fields[fi].size - os, out);
                os = 0;
                last = fi++;
            }
            continue;
        }

        // default initialize if necessary
        if (ii == nin || fi < in[ii].idx)
        {
            DUnionField& f = fields[fi];
            out.push_back(f.init);
            if (f.initsize < f.size)
                push_nulls(f.size - f.initsize, out);
            last = fi++;
            os = 0;
            continue;
        }
    }

    std::vector<const llvm::Type*> tys;
    size_t nout = out.size();
    for (size_t i=0; i<nout; ++i)
        tys.push_back(out[i]->getType());

    const llvm::StructType* st = llvm::StructType::get(tys);
    return llvm::ConstantStruct::get(st, out);
}





















