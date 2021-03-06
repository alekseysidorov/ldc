
#include "gen/llvm.h"

#include "dsymbol.h"
#include "aggregate.h"
#include "declaration.h"
#include "init.h"
#include "module.h"

#include "gen/tollvm.h"
#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/runtime.h"
#include "gen/arrays.h"
#include "gen/dvalue.h"
#include "gen/functions.h"
#include "gen/structs.h"
#include "gen/classes.h"
#include "gen/typeinf.h"
#include "gen/complex.h"
#include "gen/llvmhelpers.h"
#include "gen/linkage.h"
#include "gen/pragma.h"

#include "ir/irtype.h"
#include "ir/irtypeclass.h"
#include "ir/irtypefunction.h"

bool DtoIsPassedByRef(Type* type)
{
    Type* typ = type->toBasetype();
    TY t = typ->ty;
    return (t == Tstruct || t == Tsarray);
}

unsigned DtoShouldExtend(Type* type)
{
    type = type->toBasetype();
    if (type->isintegral())
    {
        switch(type->ty)
        {
        case Tint8:
        case Tint16:
            return llvm::Attribute::SExt;

        case Tuns8:
        case Tuns16:
            return llvm::Attribute::ZExt;
        }
    }
    return llvm::Attribute::None;
}

LLType* DtoType(Type* t)
{
    t = stripModifiers( t );

    if (t->irtype)
    {
        return t->irtype->get();
    }

    IF_LOG Logger::println("Building type: %s", t->toChars());

    assert(t);
    switch (t->ty)
    {
    // basic types
    case Tvoid:
    case Tint8:
    case Tuns8:
    case Tint16:
    case Tuns16:
    case Tint32:
    case Tuns32:
    case Tint64:
    case Tuns64:
    case Tfloat32:
    case Tfloat64:
    case Tfloat80:
    case Timaginary32:
    case Timaginary64:
    case Timaginary80:
    case Tcomplex32:
    case Tcomplex64:
    case Tcomplex80:
    //case Tbit:
    case Tbool:
    case Tchar:
    case Twchar:
    case Tdchar:
    {
        t->irtype = new IrTypeBasic(t);
        return t->irtype->buildType();
    }

    // pointers
    case Tnull:
    case Tpointer:
    {
        t->irtype = new IrTypePointer(t);
        return t->irtype->buildType();
    }

    // arrays
    case Tarray:
    {
        t->irtype = new IrTypeArray(t);
        return t->irtype->buildType();
    }

    case Tsarray:
    {
        t->irtype = new IrTypeSArray(t);
        return t->irtype->buildType();
    }

    // aggregates
    case Tstruct:    {
        TypeStruct* ts = (TypeStruct*)t;
        t->irtype = new IrTypeStruct(ts->sym);
        return t->irtype->buildType();
    }
    case Tclass:    {
        TypeClass* tc = (TypeClass*)t;
        t->irtype = new IrTypeClass(tc->sym);
        return t->irtype->buildType();
    }

    // functions
    case Tfunction:
    {
        t->irtype = new IrTypeFunction(t);
        return t->irtype->buildType();
    }

    // delegates
    case Tdelegate:
    {
        t->irtype = new IrTypeDelegate(t);
        return t->irtype->buildType();
    }

    // typedefs
    // enum

    // FIXME: maybe just call toBasetype first ?
    case Ttypedef:
    case Tenum:
    {
        Type* bt = t->toBasetype();
        assert(bt);
        return DtoType(bt);
    }

    // associative arrays
    case Taarray:
        return getVoidPtrType();

/*
    Not needed atm as VarDecls for tuples are rewritten as a string of
    VarDecls for the fields (u -> _u_field_0, ...)

    case Ttuple:
    {
        TypeTuple* ttupl = (TypeTuple*)t;
        return DtoStructTypeFromArguments(ttupl->arguments);
    }
*/

    default:
        printf("trying to convert unknown type '%s' with value %d\n", t->toChars(), t->ty);
        assert(0);
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

/*
LLType* DtoStructTypeFromArguments(Arguments* arguments)
{
    if (!arguments)
        return LLType::getVoidTy(gIR->context());

    std::vector<LLType*> types;
    for (size_t i = 0; i < arguments->dim; i++)
    {
        Argument *arg = (Argument *)arguments->data[i];
        assert(arg && arg->type);

        types.push_back(DtoType(arg->type));
    }
    return LLStructType::get(types);
}
*/

//////////////////////////////////////////////////////////////////////////////////////////

LLType* DtoTypeNotVoid(Type* t)
{
    LLType* lt = DtoType(t);
    if (lt == LLType::getVoidTy(gIR->context()))
        return LLType::getInt8Ty(gIR->context());
    return lt;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoDelegateEquals(TOK op, LLValue* lhs, LLValue* rhs)
{
    Logger::println("Doing delegate equality");
    llvm::Value *b1, *b2;
    if (rhs == NULL)
    {
        rhs = LLConstant::getNullValue(lhs->getType());
    }

    LLValue* l = gIR->ir->CreateExtractValue(lhs, 0);
    LLValue* r = gIR->ir->CreateExtractValue(rhs, 0);
    b1 = gIR->ir->CreateICmp(llvm::ICmpInst::ICMP_EQ,l,r,"tmp");

    l = gIR->ir->CreateExtractValue(lhs, 1);
    r = gIR->ir->CreateExtractValue(rhs, 1);
    b2 = gIR->ir->CreateICmp(llvm::ICmpInst::ICMP_EQ,l,r,"tmp");

    LLValue* b = gIR->ir->CreateAnd(b1,b2,"tmp");

    if (op == TOKnotequal || op == TOKnotidentity)
        return gIR->ir->CreateNot(b,"tmp");

    return b;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLGlobalValue::LinkageTypes DtoLinkage(Dsymbol* sym)
{
    // global variable
    if (VarDeclaration* vd = sym->isVarDeclaration())
    {
        if (mustDefineSymbol(vd))
            Logger::println("Variable %savailable externally: %s", (vd->availableExternally ? "" : "not "), vd->toChars());
        // generated by inlining semantics run
        if (vd->availableExternally && mustDefineSymbol(sym))
            return llvm::GlobalValue::AvailableExternallyLinkage;
        // template
        if (needsTemplateLinkage(sym))
            return templateLinkage;
        // never use InternalLinkage for variables marked as "extern"
        if (vd->storage_class & STCextern)
            return llvm::GlobalValue::ExternalLinkage;
    }
    // function
    else if (FuncDeclaration* fdecl = sym->isFuncDeclaration())
    {
        if (mustDefineSymbol(fdecl))
            Logger::println("Function %savailable externally: %s", (fdecl->availableExternally ? "" : "not "), fdecl->toChars());
        assert(fdecl->type->ty == Tfunction);
        TypeFunction* ft = (TypeFunction*)fdecl->type;

        // intrinsics are always external
        if (fdecl->llvmInternal == LLVMintrinsic)
            return llvm::GlobalValue::ExternalLinkage;
        // generated by inlining semantics run
        if (fdecl->availableExternally && mustDefineSymbol(sym))
            return llvm::GlobalValue::AvailableExternallyLinkage;
        // array operations are always template linkage
        if (fdecl->isArrayOp == 1)
            return templateLinkage;
        // template instances should have weak linkage
        // but only if there's a body, and it's not naked
        // otherwise we make it external
        else if (needsTemplateLinkage(fdecl) && fdecl->fbody && !fdecl->naked)
            return templateLinkage;
        // extern(C) functions are always external
        else if (ft->linkage == LINKc)
            return llvm::GlobalValue::ExternalLinkage;
        // If a function without a body is nested in another
        // function, we cannot use internal linkage for that
        // function (see below about nested functions)
        // FIXME: maybe there is a better way without emission
        // of needless symbols?
        if (!fdecl->fbody)
            return llvm::GlobalValue::ExternalLinkage;
    }
    // class
    else if (ClassDeclaration* cd = sym->isClassDeclaration())
    {
        if (mustDefineSymbol(cd))
            Logger::println("Class %savailable externally: %s", (cd->availableExternally ? "" : "not "), vd->toChars());
        // generated by inlining semantics run
        if (cd->availableExternally && mustDefineSymbol(sym))
            return llvm::GlobalValue::AvailableExternallyLinkage;
        // template
        if (needsTemplateLinkage(cd))
            return templateLinkage;
    }
    else
    {
        assert(0 && "not global/function");
    }

    // The following breaks for nested naked functions and other declarations, so check for that.
    bool skipNestedCheck = !mustDefineSymbol(sym);
    if (!skipNestedCheck)
        if (FuncDeclaration* fd = sym->isFuncDeclaration())
            skipNestedCheck = (fd->naked != 0);

    // Any symbol nested in a function that cannot be inlined can't be
    // referenced directly from outside that function, so we can give
    // such symbols internal linkage. This holds even if nested indirectly,
    // such as member functions of aggregates nested in functions.
    //
    // Note: This must be checked after things like template member-ness or
    // symbols nested in templates would get duplicated for each module,
    // breaking things like
    // ---
    // int counter(T)() { static int i; return i++; }"
    // ---
    // if instances get emitted in multiple object files because they'd use
    // different instances of 'i'.
    if (!skipNestedCheck) {
        for (Dsymbol* parent = sym->parent; parent ; parent = parent->parent) {
            FuncDeclaration *fd = parent->isFuncDeclaration();
            if (fd && !fd->canInline(fd->needThis()))
                return llvm::GlobalValue::InternalLinkage;
        }
    }

    // default to external linkage
    return llvm::GlobalValue::ExternalLinkage;
}

static bool isAvailableExternally(Dsymbol* sym)
{
    if (VarDeclaration* vd = sym->isVarDeclaration())
        return vd->availableExternally;
    if (FuncDeclaration* fd = sym->isFuncDeclaration())
        return fd->availableExternally;
    if (AggregateDeclaration* ad = sym->isAggregateDeclaration())
        return ad->availableExternally;
    return false;
}

llvm::GlobalValue::LinkageTypes DtoInternalLinkage(Dsymbol* sym)
{
    if (needsTemplateLinkage(sym)) {
        if (isAvailableExternally(sym) && mustDefineSymbol(sym))
            return llvm::GlobalValue::AvailableExternallyLinkage;
        return templateLinkage;
    }
    else
        return llvm::GlobalValue::InternalLinkage;
}

llvm::GlobalValue::LinkageTypes DtoExternalLinkage(Dsymbol* sym)
{
    if (needsTemplateLinkage(sym))
        return templateLinkage;
    else if (isAvailableExternally(sym) && mustDefineSymbol(sym))
        return llvm::GlobalValue::AvailableExternallyLinkage;
    else
        return llvm::GlobalValue::ExternalLinkage;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoPointedType(LLValue* ptr, LLValue* val)
{
    LLType* ptrTy = ptr->getType()->getContainedType(0);
    LLType* valTy = val->getType();
    // ptr points to val's type
    if (ptrTy == valTy)
    {
        return val;
    }
    // ptr is integer pointer
    else if (ptrTy->isIntegerTy())
    {
        // val is integer
        assert(valTy->isIntegerTy());
        LLIntegerType* pt = llvm::cast<LLIntegerType>(ptrTy);
        LLIntegerType* vt = llvm::cast<LLIntegerType>(valTy);
        if (pt->getBitWidth() < vt->getBitWidth()) {
            return new llvm::TruncInst(val, pt, "tmp", gIR->scopebb());
        }
        else
        assert(0);
    }
    // something else unsupported
    else
    {
        if (Logger::enabled())
            Logger::cout() << *ptrTy << '|' << *valTy << '\n';
        assert(0);
    }
    return 0;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLIntegerType* DtoSize_t()
{
    // the type of size_t does not change once set
    static LLIntegerType* t = NULL;
    if (t == NULL)
        t = (global.params.is64bit) ? LLType::getInt64Ty(gIR->context()) : LLType::getInt32Ty(gIR->context());
    return t;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoGEP1(LLValue* ptr, LLValue* i0, const char* var, llvm::BasicBlock* bb)
{
    return llvm::GetElementPtrInst::Create(ptr, i0, var?var:"tmp", bb?bb:gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoGEP(LLValue* ptr, LLValue* i0, LLValue* i1, const char* var, llvm::BasicBlock* bb)
{
    LLSmallVector<LLValue*,2> v(2);
    v[0] = i0;
    v[1] = i1;
    return llvm::GetElementPtrInst::Create(ptr, v, var?var:"tmp", bb?bb:gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoGEPi1(LLValue* ptr, unsigned i, const char* var, llvm::BasicBlock* bb)
{
    return llvm::GetElementPtrInst::Create(ptr, DtoConstUint(i), var?var:"tmp", bb?bb:gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoGEPi(LLValue* ptr, unsigned i0, unsigned i1, const char* var, llvm::BasicBlock* bb)
{
    LLSmallVector<LLValue*,2> v(2);
    v[0] = DtoConstUint(i0);
    v[1] = DtoConstUint(i1);
    return llvm::GetElementPtrInst::Create(ptr, v, var?var:"tmp", bb?bb:gIR->scopebb());
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* DtoGEPi(LLConstant* ptr, unsigned i0, unsigned i1)
{
    LLValue* v[2] = { DtoConstUint(i0), DtoConstUint(i1) };
    return llvm::ConstantExpr::getGetElementPtr(ptr, v, true);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoMemSet(LLValue* dst, LLValue* val, LLValue* nbytes)
{
    dst = DtoBitCast(dst,getVoidPtrType());

    LLType* intTy = DtoSize_t();
    LLType *VoidPtrTy = getVoidPtrType();
    LLType *Tys[2] ={VoidPtrTy, intTy};
    llvm::Function* fn = llvm::Intrinsic::getDeclaration(gIR->module,
                llvm::Intrinsic::memset, llvm::makeArrayRef(Tys, 2));
    
    gIR->ir->CreateCall5(fn, dst, val, nbytes, DtoConstUint(1), DtoConstBool(false), "");
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoMemSetZero(LLValue* dst, LLValue* nbytes)
{
    DtoMemSet(dst, DtoConstUbyte(0), nbytes);
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoMemCpy(LLValue* dst, LLValue* src, LLValue* nbytes, unsigned align)
{
    dst = DtoBitCast(dst,getVoidPtrType());
    src = DtoBitCast(src,getVoidPtrType());

    LLType* intTy = DtoSize_t();
    LLType *VoidPtrTy = getVoidPtrType();
    LLType *Tys[3] ={VoidPtrTy, VoidPtrTy, intTy};
    llvm::Function* fn = llvm::Intrinsic::getDeclaration(gIR->module,
        llvm::Intrinsic::memcpy, llvm::makeArrayRef(Tys, 3));
    
    gIR->ir->CreateCall5(fn, dst, src, nbytes, DtoConstUint(align), DtoConstBool(false), "");
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoMemCmp(LLValue* lhs, LLValue* rhs, LLValue* nbytes)
{
    // int memcmp ( const void * ptr1, const void * ptr2, size_t num );

    LLFunction* fn = gIR->module->getFunction("memcmp");
    if (!fn)
    {
        std::vector<LLType*> params(3);
        params[0] = getVoidPtrType();
        params[1] = getVoidPtrType();
        params[2] = DtoSize_t();
        LLFunctionType* fty = LLFunctionType::get(LLType::getInt32Ty(gIR->context()), params, false);
        fn = LLFunction::Create(fty, LLGlobalValue::ExternalLinkage, "memcmp", gIR->module);
    }

    lhs = DtoBitCast(lhs,getVoidPtrType());
    rhs = DtoBitCast(rhs,getVoidPtrType());

    return gIR->ir->CreateCall3(fn, lhs, rhs, nbytes, "tmp");
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoAggrZeroInit(LLValue* v)
{
    uint64_t n = getTypeStoreSize(v->getType()->getContainedType(0));
    DtoMemSetZero(v, DtoConstSize_t(n));
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoAggrCopy(LLValue* dst, LLValue* src)
{
    uint64_t n = getTypeStoreSize(dst->getType()->getContainedType(0));
    DtoMemCpy(dst, src, DtoConstSize_t(n));
}

//////////////////////////////////////////////////////////////////////////////////////////

void DtoMemoryBarrier(bool ll, bool ls, bool sl, bool ss, bool device)
{
    // FIXME: implement me
    /*llvm::Function* fn = GET_INTRINSIC_DECL(memory_barrier);
    assert(fn != NULL);

    LLSmallVector<LLValue*, 5> llargs;
    llargs.push_back(DtoConstBool(ll));
    llargs.push_back(DtoConstBool(ls));
    llargs.push_back(DtoConstBool(sl));
    llargs.push_back(DtoConstBool(ss));
    llargs.push_back(DtoConstBool(device));

    llvm::CallInst::Create(fn, llargs, "", gIR->scopebb());*/
}

//////////////////////////////////////////////////////////////////////////////////////////

llvm::ConstantInt* DtoConstSize_t(uint64_t i)
{
    return LLConstantInt::get(DtoSize_t(), i, false);
}
llvm::ConstantInt* DtoConstUint(unsigned i)
{
    return LLConstantInt::get(LLType::getInt32Ty(gIR->context()), i, false);
}
llvm::ConstantInt* DtoConstInt(int i)
{
    return LLConstantInt::get(LLType::getInt32Ty(gIR->context()), i, true);
}
LLConstant* DtoConstBool(bool b)
{
    return LLConstantInt::get(LLType::getInt1Ty(gIR->context()), b, false);
}
llvm::ConstantInt* DtoConstUbyte(unsigned char i)
{
    return LLConstantInt::get(LLType::getInt8Ty(gIR->context()), i, false);
}

LLConstant* DtoConstFP(Type* t, long double value)
{
    LLType* llty = DtoType(t);
    assert(llty->isFloatingPointTy());

    if(llty == LLType::getFloatTy(gIR->context()) || llty == LLType::getDoubleTy(gIR->context()))
        return LLConstantFP::get(llty, value);
    else if(llty == LLType::getX86_FP80Ty(gIR->context())) {
        uint64_t bits[] = {0, 0};
        bits[0] = *(uint64_t*)&value;
        bits[1] = *(uint16_t*)((uint64_t*)&value + 1);
        return LLConstantFP::get(gIR->context(), APFloat(APInt(80, 2, bits)));
    } else {
        assert(0 && "Unknown floating point type encountered");
    }
}

//////////////////////////////////////////////////////////////////////////////////////////

LLConstant* DtoConstString(const char* str)
{
    llvm::StringRef s(str?str:"");
    LLConstant* init = LLConstantArray::get(gIR->context(), s, true);
    llvm::GlobalVariable* gvar = new llvm::GlobalVariable(
        *gIR->module, init->getType(), true,llvm::GlobalValue::InternalLinkage, init, ".str");
    LLConstant* idxs[2] = { DtoConstUint(0), DtoConstUint(0) };
    return DtoConstSlice(
        DtoConstSize_t(s.size()),
        llvm::ConstantExpr::getGetElementPtr(gvar, idxs, true),
        Type::tchar->arrayOf()
    );
}
LLConstant* DtoConstStringPtr(const char* str, const char* section)
{
    llvm::StringRef s(str);
    LLConstant* init = LLConstantArray::get(gIR->context(), s, true);
    llvm::GlobalVariable* gvar = new llvm::GlobalVariable(
        *gIR->module, init->getType(), true,llvm::GlobalValue::InternalLinkage, init, ".str");
    if (section) gvar->setSection(section);
    LLConstant* idxs[2] = { DtoConstUint(0), DtoConstUint(0) };
    return llvm::ConstantExpr::getGetElementPtr(gvar, idxs, true);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoLoad(LLValue* src, const char* name)
{
//     if (Logger::enabled())
//         Logger::cout() << "loading " << *src <<  '\n';
    llvm::LoadInst* ld = gIR->ir->CreateLoad(src, name ? name : "tmp");
    //ld->setVolatile(gIR->func()->inVolatile);
    return ld;
}

// Like DtoLoad, but the pointer is guaranteed to be aligned appropriately for the type.
LLValue* DtoAlignedLoad(LLValue* src, const char* name)
{
    llvm::LoadInst* ld = gIR->ir->CreateLoad(src, name ? name : "tmp");
    ld->setAlignment(getABITypeAlign(ld->getType()));
    return ld;
}


void DtoStore(LLValue* src, LLValue* dst)
{
//     if (Logger::enabled())
//         Logger::cout() << "storing " << *src << " into " << *dst << '\n';
    LLValue* st = gIR->ir->CreateStore(src,dst);
    //st->setVolatile(gIR->func()->inVolatile);
}

// Like DtoStore, but the pointer is guaranteed to be aligned appropriately for the type.
void DtoAlignedStore(LLValue* src, LLValue* dst)
{
    llvm::StoreInst* st = gIR->ir->CreateStore(src,dst);
    st->setAlignment(getABITypeAlign(src->getType()));
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoBitCast(LLValue* v, LLType* t, const char* name)
{
    if (v->getType() == t)
        return v;
    assert(!isaStruct(t));
    return gIR->ir->CreateBitCast(v, t, name ? name : "tmp");
}

LLConstant* DtoBitCast(LLConstant* v, LLType* t)
{
    if (v->getType() == t)
        return v;
    return llvm::ConstantExpr::getBitCast(v, t);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoInsertValue(LLValue* aggr, LLValue* v, unsigned idx, const char* name)
{
    return gIR->ir->CreateInsertValue(aggr, v, idx, name ? name : "tmp");
}

LLValue* DtoExtractValue(LLValue* aggr, unsigned idx, const char* name)
{
    return gIR->ir->CreateExtractValue(aggr, idx, name ? name : "tmp");
}

//////////////////////////////////////////////////////////////////////////////////////////

LLPointerType* isaPointer(LLValue* v)
{
    return llvm::dyn_cast<LLPointerType>(v->getType());
}

LLPointerType* isaPointer(LLType* t)
{
    return llvm::dyn_cast<LLPointerType>(t);
}

LLArrayType* isaArray(LLValue* v)
{
    return llvm::dyn_cast<LLArrayType>(v->getType());
}

LLArrayType* isaArray(LLType* t)
{
    return llvm::dyn_cast<LLArrayType>(t);
}

LLStructType* isaStruct(LLValue* v)
{
    return llvm::dyn_cast<LLStructType>(v->getType());
}

LLStructType* isaStruct(LLType* t)
{
    return llvm::dyn_cast<LLStructType>(t);
}

LLFunctionType* isaFunction(LLValue* v)
{
    return llvm::dyn_cast<LLFunctionType>(v->getType());
}

LLFunctionType* isaFunction(LLType* t)
{
    return llvm::dyn_cast<LLFunctionType>(t);
}

LLConstant* isaConstant(LLValue* v)
{
    return llvm::dyn_cast<llvm::Constant>(v);
}

llvm::ConstantInt* isaConstantInt(LLValue* v)
{
    return llvm::dyn_cast<llvm::ConstantInt>(v);
}

llvm::Argument* isaArgument(LLValue* v)
{
    return llvm::dyn_cast<llvm::Argument>(v);
}

llvm::GlobalVariable* isaGlobalVar(LLValue* v)
{
    return llvm::dyn_cast<llvm::GlobalVariable>(v);
}

//////////////////////////////////////////////////////////////////////////////////////////

LLPointerType* getPtrToType(LLType* t)
{
    if (t == LLType::getVoidTy(gIR->context()))
        t = LLType::getInt8Ty(gIR->context());
    return LLPointerType::get(t, 0);
}

LLPointerType* getVoidPtrType()
{
    return getPtrToType(LLType::getInt8Ty(gIR->context()));
}

llvm::ConstantPointerNull* getNullPtr(LLType* t)
{
    LLPointerType* pt = llvm::cast<LLPointerType>(t);
    return llvm::ConstantPointerNull::get(pt);
}

LLConstant* getNullValue(LLType* t)
{
    return LLConstant::getNullValue(t);
}

//////////////////////////////////////////////////////////////////////////////////////////

size_t getTypeBitSize(LLType* t)
{
    return gTargetData->getTypeSizeInBits(t);
}

size_t getTypeStoreSize(LLType* t)
{
    return gTargetData->getTypeStoreSize(t);
}

size_t getTypePaddedSize(LLType* t)
{
    size_t sz = gTargetData->getTypeAllocSize(t);
    //Logger::cout() << "abi type size of: " << *t << " == " << sz << '\n';
    return sz;
}

size_t getTypeAllocSize(LLType* t)
{
    return gTargetData->getTypeAllocSize(t);
}

unsigned char getABITypeAlign(LLType* t)
{
    return gTargetData->getABITypeAlignment(t);
}

unsigned char getPrefTypeAlign(LLType* t)
{
    return gTargetData->getPrefTypeAlignment(t);
}

LLType* getBiggestType(LLType** begin, size_t n)
{
    LLType* bigTy = 0;
    size_t bigSize = 0;
    size_t bigAlign = 0;

    LLType** end = begin+n;
    while (begin != end)
    {
        LLType* T = *begin;

        size_t sz = getTypePaddedSize(T);
        size_t ali = getABITypeAlign(T);
        if (sz > bigSize || (sz == bigSize && ali > bigAlign))
        {
            bigTy = T;
            bigSize = sz;
            bigAlign = ali;
        }

        ++begin;
    }

    // will be null for n==0
    return bigTy;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLStructType* DtoInterfaceInfoType()
{
    if (gIR->interfaceInfoType)
        return gIR->interfaceInfoType;

    // build interface info type
    std::vector<LLType*> types;
    // ClassInfo classinfo
    ClassDeclaration* cd2 = ClassDeclaration::classinfo;
    DtoResolveClass(cd2);
    types.push_back(DtoType(cd2->type));
    // void*[] vtbl
    std::vector<LLType*> vtbltypes;
    vtbltypes.push_back(DtoSize_t());
    LLType* byteptrptrty = getPtrToType(getPtrToType(LLType::getInt8Ty(gIR->context())));
    vtbltypes.push_back(byteptrptrty);
    types.push_back(LLStructType::get(gIR->context(), vtbltypes));
    // int offset
    types.push_back(LLType::getInt32Ty(gIR->context()));
    // create type
    gIR->interfaceInfoType = LLStructType::get(gIR->context(), types);

    return gIR->interfaceInfoType;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLStructType* DtoMutexType()
{
    if (gIR->mutexType)
        return gIR->mutexType;

    // win32
    if (global.params.os == OSWindows)
    {
        // CRITICAL_SECTION.sizeof == 68
        std::vector<LLType*> types(17, LLType::getInt32Ty(gIR->context()));
        return LLStructType::get(gIR->context(), types);
    }

    // FreeBSD
    else if (global.params.os == OSFreeBSD) {
        // Just a pointer
        return LLStructType::get(gIR->context(), DtoSize_t(), NULL);
    }

    // pthread_fastlock
    std::vector<LLType*> types2;
    types2.push_back(DtoSize_t());
    types2.push_back(LLType::getInt32Ty(gIR->context()));
    LLStructType* fastlock = LLStructType::get(gIR->context(), types2);

    // pthread_mutex
    std::vector<LLType*> types1;
    types1.push_back(LLType::getInt32Ty(gIR->context()));
    types1.push_back(LLType::getInt32Ty(gIR->context()));
    types1.push_back(getVoidPtrType());
    types1.push_back(LLType::getInt32Ty(gIR->context()));
    types1.push_back(fastlock);
    LLStructType* pmutex = LLStructType::get(gIR->context(), types1);

    // D_CRITICAL_SECTION
    LLStructType* mutex = LLStructType::create(gIR->context(), "D_CRITICAL_SECTION");
    std::vector<LLType*> types;
    types.push_back(getPtrToType(mutex));
    types.push_back(pmutex);
    mutex->setBody(types);
    gIR->mutexType = mutex;

    return pmutex;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLStructType* DtoModuleReferenceType()
{
    if (gIR->moduleRefType)
        return gIR->moduleRefType;

    // this is a recursive type so start out with a struct without body
    LLStructType* st = LLStructType::create(gIR->context(), "ModuleReference");

    // add members
    std::vector<LLType*> types;
    types.push_back(getPtrToType(st));
#if DMDV1
    types.push_back(DtoType(Module::moduleinfo->type));
#else
    types.push_back(DtoType(Module::moduleinfo->type->pointerTo()));
#endif

    // resolve type
    st->setBody(types);

    // done
    gIR->moduleRefType = st;
    return st;
}

//////////////////////////////////////////////////////////////////////////////////////////

LLValue* DtoAggrPair(LLType* type, LLValue* V1, LLValue* V2, const char* name)
{
    LLValue* res = llvm::UndefValue::get(type);
    res = gIR->ir->CreateInsertValue(res, V1, 0, "tmp");
    return gIR->ir->CreateInsertValue(res, V2, 1, name?name:"tmp");
}

LLValue* DtoAggrPair(LLValue* V1, LLValue* V2, const char* name)
{
    llvm::SmallVector<LLType*, 2> types;
    types.push_back(V1->getType());
    types.push_back(V2->getType());
    LLType* t = LLStructType::get(gIR->context(), types);
    return DtoAggrPair(t, V1, V2, name);
}

LLValue* DtoAggrPaint(LLValue* aggr, LLType* as)
{
    if (aggr->getType() == as)
        return aggr;

    LLValue* res = llvm::UndefValue::get(as);

    LLValue* V = gIR->ir->CreateExtractValue(aggr, 0, "tmp");;
    V = DtoBitCast(V, as->getContainedType(0));
    res = gIR->ir->CreateInsertValue(res, V, 0, "tmp");

    V = gIR->ir->CreateExtractValue(aggr, 1, "tmp");;
    V = DtoBitCast(V, as->getContainedType(1));
    return gIR->ir->CreateInsertValue(res, V, 1, "tmp");
}

LLValue* DtoAggrPairSwap(LLValue* aggr)
{
    Logger::println("swapping aggr pair");
    LLValue* r = gIR->ir->CreateExtractValue(aggr, 0);
    LLValue* i = gIR->ir->CreateExtractValue(aggr, 1);
    return DtoAggrPair(i, r, "swapped");
}
