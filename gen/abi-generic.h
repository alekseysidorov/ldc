#ifndef LDC_GEN_ABI_GENERIC
#define LDC_GEN_ABI_GENERIC

#include "gen/llvmhelpers.h"
#include "gen/tollvm.h"
#include "gen/structs.h"

/// Removes padding fields for (non-union-containing!) structs
struct RemoveStructPadding : ABIRewrite {
    /// get a rewritten value back to its original form
    virtual LLValue* get(Type* dty, DValue* v) {
        LLValue* lval = DtoAlloca(dty, ".rewritetmp");
        getL(dty, v, lval);
        return lval;
    }

    /// get a rewritten value back to its original form and store result in provided lvalue
    /// this one is optional and defaults to calling the one above
    virtual void getL(Type* dty, DValue* v, llvm::Value* lval) {
        // Make sure the padding is zero, so struct comparisons work.
        // TODO: Only do this if there's padding, and/or only initialize padding.
        DtoMemSetZero(lval, DtoConstSize_t(getTypePaddedSize(DtoType(dty))));
        DtoPaddedStruct(dty->toBasetype(), v->getRVal(), lval);
    }

    /// put out rewritten value
    virtual LLValue* put(Type* dty, DValue* v) {
        return DtoUnpaddedStruct(dty->toBasetype(), v->getRVal());
    }

    /// return the transformed type for this rewrite
    virtual LLType* type(Type* dty, LLType* t) {
        return DtoUnpaddedStructType(dty->toBasetype());
    }
};

//////////////////////////////////////////////////////////////////////////////

// simply swap of real/imag parts for proper x87 complex abi
struct X87_complex_swap : ABIRewrite
{
    LLValue* get(Type*, DValue* v)
    {
        return DtoAggrPairSwap(v->getRVal());
    }
    LLValue* put(Type*, DValue* v)
    {
        return DtoAggrPairSwap(v->getRVal());
    }
    LLType* type(Type*, LLType* t)
    {
        return t;
    }
};

//////////////////////////////////////////////////////////////////////////////

// FIXME: try into eliminating the alloca or if at least check
// if it gets optimized away

// convert byval struct
// when
struct X86_struct_to_register : ABIRewrite
{
    // int -> struct
    LLValue* get(Type* dty, DValue* dv)
    {
        Logger::println("rewriting int -> struct");
        LLValue* mem = DtoAlloca(dty, ".int_to_struct");
        LLValue* v = dv->getRVal();
        DtoStore(v, DtoBitCast(mem, getPtrToType(v->getType())));
        return DtoLoad(mem);
    }
    // int -> struct (with dst lvalue given)
    void getL(Type* dty, DValue* dv, llvm::Value* lval)
    {
        Logger::println("rewriting int -> struct");
        LLValue* v = dv->getRVal();
        DtoStore(v, DtoBitCast(lval, getPtrToType(v->getType())));
    }
    // struct -> int
    LLValue* put(Type* dty, DValue* dv)
    {
        Logger::println("rewriting struct -> int");
        assert(dv->isLVal());
        LLValue* mem = dv->getLVal();
        LLType* t = LLIntegerType::get(gIR->context(), dty->size()*8);
        return DtoLoad(DtoBitCast(mem, getPtrToType(t)));
    }
    LLType* type(Type* t, LLType*)
    {
        size_t sz = t->size()*8;
        return LLIntegerType::get(gIR->context(), sz);
    }
};

#endif
