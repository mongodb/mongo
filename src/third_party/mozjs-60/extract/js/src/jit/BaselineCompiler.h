/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineCompiler_h
#define jit_BaselineCompiler_h

#include "jit/FixedList.h"
#if defined(JS_CODEGEN_X86)
# include "jit/x86/BaselineCompiler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/BaselineCompiler-x64.h"
#elif defined(JS_CODEGEN_ARM)
# include "jit/arm/BaselineCompiler-arm.h"
#elif defined(JS_CODEGEN_ARM64)
# include "jit/arm64/BaselineCompiler-arm64.h"
#elif defined(JS_CODEGEN_MIPS32)
# include "jit/mips32/BaselineCompiler-mips32.h"
#elif defined(JS_CODEGEN_MIPS64)
# include "jit/mips64/BaselineCompiler-mips64.h"
#elif defined(JS_CODEGEN_NONE)
# include "jit/none/BaselineCompiler-none.h"
#else
# error "Unknown architecture!"
#endif

namespace js {
namespace jit {

#define OPCODE_LIST(_)         \
    _(JSOP_NOP)                \
    _(JSOP_NOP_DESTRUCTURING)  \
    _(JSOP_LABEL)              \
    _(JSOP_ITERNEXT)           \
    _(JSOP_POP)                \
    _(JSOP_POPN)               \
    _(JSOP_DUPAT)              \
    _(JSOP_ENTERWITH)          \
    _(JSOP_LEAVEWITH)          \
    _(JSOP_DUP)                \
    _(JSOP_DUP2)               \
    _(JSOP_SWAP)               \
    _(JSOP_PICK)               \
    _(JSOP_UNPICK)             \
    _(JSOP_GOTO)               \
    _(JSOP_IFEQ)               \
    _(JSOP_IFNE)               \
    _(JSOP_AND)                \
    _(JSOP_OR)                 \
    _(JSOP_NOT)                \
    _(JSOP_POS)                \
    _(JSOP_LOOPHEAD)           \
    _(JSOP_LOOPENTRY)          \
    _(JSOP_VOID)               \
    _(JSOP_UNDEFINED)          \
    _(JSOP_HOLE)               \
    _(JSOP_NULL)               \
    _(JSOP_TRUE)               \
    _(JSOP_FALSE)              \
    _(JSOP_ZERO)               \
    _(JSOP_ONE)                \
    _(JSOP_INT8)               \
    _(JSOP_INT32)              \
    _(JSOP_UINT16)             \
    _(JSOP_UINT24)             \
    _(JSOP_DOUBLE)             \
    _(JSOP_STRING)             \
    _(JSOP_SYMBOL)             \
    _(JSOP_OBJECT)             \
    _(JSOP_CALLSITEOBJ)        \
    _(JSOP_REGEXP)             \
    _(JSOP_LAMBDA)             \
    _(JSOP_LAMBDA_ARROW)       \
    _(JSOP_SETFUNNAME)         \
    _(JSOP_BITOR)              \
    _(JSOP_BITXOR)             \
    _(JSOP_BITAND)             \
    _(JSOP_LSH)                \
    _(JSOP_RSH)                \
    _(JSOP_URSH)               \
    _(JSOP_ADD)                \
    _(JSOP_SUB)                \
    _(JSOP_MUL)                \
    _(JSOP_DIV)                \
    _(JSOP_MOD)                \
    _(JSOP_POW)                \
    _(JSOP_LT)                 \
    _(JSOP_LE)                 \
    _(JSOP_GT)                 \
    _(JSOP_GE)                 \
    _(JSOP_EQ)                 \
    _(JSOP_NE)                 \
    _(JSOP_STRICTEQ)           \
    _(JSOP_STRICTNE)           \
    _(JSOP_CONDSWITCH)         \
    _(JSOP_CASE)               \
    _(JSOP_DEFAULT)            \
    _(JSOP_LINENO)             \
    _(JSOP_BITNOT)             \
    _(JSOP_NEG)                \
    _(JSOP_NEWARRAY)           \
    _(JSOP_NEWARRAY_COPYONWRITE) \
    _(JSOP_INITELEM_ARRAY)     \
    _(JSOP_NEWOBJECT)          \
    _(JSOP_NEWINIT)            \
    _(JSOP_INITELEM)           \
    _(JSOP_INITELEM_GETTER)    \
    _(JSOP_INITELEM_SETTER)    \
    _(JSOP_INITELEM_INC)       \
    _(JSOP_MUTATEPROTO)        \
    _(JSOP_INITPROP)           \
    _(JSOP_INITLOCKEDPROP)     \
    _(JSOP_INITHIDDENPROP)     \
    _(JSOP_INITPROP_GETTER)    \
    _(JSOP_INITPROP_SETTER)    \
    _(JSOP_GETELEM)            \
    _(JSOP_SETELEM)            \
    _(JSOP_STRICTSETELEM)      \
    _(JSOP_CALLELEM)           \
    _(JSOP_DELELEM)            \
    _(JSOP_STRICTDELELEM)      \
    _(JSOP_GETELEM_SUPER)      \
    _(JSOP_SETELEM_SUPER)      \
    _(JSOP_STRICTSETELEM_SUPER) \
    _(JSOP_IN)                 \
    _(JSOP_HASOWN)             \
    _(JSOP_GETGNAME)           \
    _(JSOP_BINDGNAME)          \
    _(JSOP_SETGNAME)           \
    _(JSOP_STRICTSETGNAME)     \
    _(JSOP_SETNAME)            \
    _(JSOP_STRICTSETNAME)      \
    _(JSOP_GETPROP)            \
    _(JSOP_SETPROP)            \
    _(JSOP_STRICTSETPROP)      \
    _(JSOP_CALLPROP)           \
    _(JSOP_DELPROP)            \
    _(JSOP_STRICTDELPROP)      \
    _(JSOP_GETPROP_SUPER)      \
    _(JSOP_SETPROP_SUPER)      \
    _(JSOP_STRICTSETPROP_SUPER) \
    _(JSOP_LENGTH)             \
    _(JSOP_GETBOUNDNAME)       \
    _(JSOP_GETALIASEDVAR)      \
    _(JSOP_SETALIASEDVAR)      \
    _(JSOP_GETNAME)            \
    _(JSOP_BINDNAME)           \
    _(JSOP_DELNAME)            \
    _(JSOP_GETIMPORT)          \
    _(JSOP_GETINTRINSIC)       \
    _(JSOP_BINDVAR)            \
    _(JSOP_DEFVAR)             \
    _(JSOP_DEFCONST)           \
    _(JSOP_DEFLET)             \
    _(JSOP_DEFFUN)             \
    _(JSOP_GETLOCAL)           \
    _(JSOP_SETLOCAL)           \
    _(JSOP_GETARG)             \
    _(JSOP_SETARG)             \
    _(JSOP_CHECKLEXICAL)       \
    _(JSOP_INITLEXICAL)        \
    _(JSOP_INITGLEXICAL)       \
    _(JSOP_CHECKALIASEDLEXICAL) \
    _(JSOP_INITALIASEDLEXICAL) \
    _(JSOP_UNINITIALIZED)      \
    _(JSOP_CALL)               \
    _(JSOP_CALL_IGNORES_RV)    \
    _(JSOP_CALLITER)           \
    _(JSOP_FUNCALL)            \
    _(JSOP_FUNAPPLY)           \
    _(JSOP_NEW)                \
    _(JSOP_EVAL)               \
    _(JSOP_STRICTEVAL)         \
    _(JSOP_SPREADCALL)         \
    _(JSOP_SPREADNEW)          \
    _(JSOP_SPREADEVAL)         \
    _(JSOP_STRICTSPREADEVAL)   \
    _(JSOP_OPTIMIZE_SPREADCALL)\
    _(JSOP_IMPLICITTHIS)       \
    _(JSOP_GIMPLICITTHIS)      \
    _(JSOP_INSTANCEOF)         \
    _(JSOP_TYPEOF)             \
    _(JSOP_TYPEOFEXPR)         \
    _(JSOP_THROWMSG)           \
    _(JSOP_THROW)              \
    _(JSOP_THROWING)           \
    _(JSOP_TRY)                \
    _(JSOP_FINALLY)            \
    _(JSOP_GOSUB)              \
    _(JSOP_RETSUB)             \
    _(JSOP_PUSHLEXICALENV)     \
    _(JSOP_POPLEXICALENV)      \
    _(JSOP_FRESHENLEXICALENV)  \
    _(JSOP_RECREATELEXICALENV) \
    _(JSOP_DEBUGLEAVELEXICALENV) \
    _(JSOP_PUSHVARENV)         \
    _(JSOP_POPVARENV)          \
    _(JSOP_EXCEPTION)          \
    _(JSOP_DEBUGGER)           \
    _(JSOP_ARGUMENTS)          \
    _(JSOP_RUNONCE)            \
    _(JSOP_REST)               \
    _(JSOP_TOASYNC)            \
    _(JSOP_TOASYNCGEN)         \
    _(JSOP_TOASYNCITER)        \
    _(JSOP_TOID)               \
    _(JSOP_TOSTRING)           \
    _(JSOP_TABLESWITCH)        \
    _(JSOP_ITER)               \
    _(JSOP_MOREITER)           \
    _(JSOP_ISNOITER)           \
    _(JSOP_ENDITER)            \
    _(JSOP_ISGENCLOSING)       \
    _(JSOP_GENERATOR)          \
    _(JSOP_INITIALYIELD)       \
    _(JSOP_YIELD)              \
    _(JSOP_AWAIT)              \
    _(JSOP_DEBUGAFTERYIELD)    \
    _(JSOP_FINALYIELDRVAL)     \
    _(JSOP_RESUME)             \
    _(JSOP_CALLEE)             \
    _(JSOP_SUPERBASE)          \
    _(JSOP_SUPERFUN)           \
    _(JSOP_GETRVAL)            \
    _(JSOP_SETRVAL)            \
    _(JSOP_RETRVAL)            \
    _(JSOP_RETURN)             \
    _(JSOP_FUNCTIONTHIS)       \
    _(JSOP_GLOBALTHIS)         \
    _(JSOP_CHECKISOBJ)         \
    _(JSOP_CHECKISCALLABLE)    \
    _(JSOP_CHECKTHIS)          \
    _(JSOP_CHECKTHISREINIT)    \
    _(JSOP_CHECKRETURN)        \
    _(JSOP_NEWTARGET)          \
    _(JSOP_SUPERCALL)          \
    _(JSOP_SPREADSUPERCALL)    \
    _(JSOP_THROWSETCONST)      \
    _(JSOP_THROWSETALIASEDCONST) \
    _(JSOP_THROWSETCALLEE)     \
    _(JSOP_INITHIDDENPROP_GETTER) \
    _(JSOP_INITHIDDENPROP_SETTER) \
    _(JSOP_INITHIDDENELEM)     \
    _(JSOP_INITHIDDENELEM_GETTER) \
    _(JSOP_INITHIDDENELEM_SETTER) \
    _(JSOP_CHECKOBJCOERCIBLE)  \
    _(JSOP_DEBUGCHECKSELFHOSTED) \
    _(JSOP_JUMPTARGET)         \
    _(JSOP_IS_CONSTRUCTING)    \
    _(JSOP_TRY_DESTRUCTURING_ITERCLOSE) \
    _(JSOP_CHECKCLASSHERITAGE) \
    _(JSOP_INITHOMEOBJECT)     \
    _(JSOP_BUILTINPROTO)       \
    _(JSOP_OBJWITHPROTO)       \
    _(JSOP_FUNWITHPROTO)       \
    _(JSOP_CLASSCONSTRUCTOR)   \
    _(JSOP_DERIVEDCONSTRUCTOR)

class BaselineCompiler : public BaselineCompilerSpecific
{
    FixedList<Label>            labels_;
    NonAssertingLabel           return_;
    NonAssertingLabel           postBarrierSlot_;

    // Native code offset right before the scope chain is initialized.
    CodeOffset prologueOffset_;

    // Native code offset right before the frame is popped and the method
    // returned from.
    CodeOffset epilogueOffset_;

    // Native code offset right after debug prologue and epilogue, or
    // equivalent positions when debug mode is off.
    CodeOffset postDebugPrologueOffset_;

    // For each INITIALYIELD or YIELD or AWAIT op, this Vector maps the yield
    // index to the bytecode offset of the next op.
    Vector<uint32_t>            yieldAndAwaitOffsets_;

    // Whether any on stack arguments are modified.
    bool modifiesArguments_;

    Label* labelOf(jsbytecode* pc) {
        return &labels_[script->pcToOffset(pc)];
    }

    // If a script has more |nslots| than this, then emit code to do an
    // early stack check.
    static const unsigned EARLY_STACK_CHECK_SLOT_COUNT = 128;
    bool needsEarlyStackCheck() const {
        return script->nslots() > EARLY_STACK_CHECK_SLOT_COUNT;
    }

  public:
    BaselineCompiler(JSContext* cx, TempAllocator& alloc, JSScript* script);
    MOZ_MUST_USE bool init();

    MethodStatus compile();

  private:
    MethodStatus emitBody();

    MOZ_MUST_USE bool emitCheckThis(ValueOperand val, bool reinit=false);
    void emitLoadReturnValue(ValueOperand val);

    void emitInitializeLocals();
    MOZ_MUST_USE bool emitPrologue();
    MOZ_MUST_USE bool emitEpilogue();
    MOZ_MUST_USE bool emitOutOfLinePostBarrierSlot();
    MOZ_MUST_USE bool emitIC(ICStub* stub, ICEntry::Kind kind);
    MOZ_MUST_USE bool emitOpIC(ICStub* stub) {
        return emitIC(stub, ICEntry::Kind_Op);
    }
    MOZ_MUST_USE bool emitNonOpIC(ICStub* stub) {
        return emitIC(stub, ICEntry::Kind_NonOp);
    }

    MOZ_MUST_USE bool emitStackCheck(bool earlyCheck=false);
    MOZ_MUST_USE bool emitInterruptCheck();
    MOZ_MUST_USE bool emitWarmUpCounterIncrement(bool allowOsr=true);
    MOZ_MUST_USE bool emitArgumentTypeChecks();
    void emitIsDebuggeeCheck();
    MOZ_MUST_USE bool emitDebugPrologue();
    MOZ_MUST_USE bool emitDebugTrap();
    MOZ_MUST_USE bool emitTraceLoggerEnter();
    MOZ_MUST_USE bool emitTraceLoggerExit();
    MOZ_MUST_USE bool emitTraceLoggerResume(Register script, AllocatableGeneralRegisterSet& regs);

    void emitProfilerEnterFrame();
    void emitProfilerExitFrame();

    MOZ_MUST_USE bool initEnvironmentChain();

    void storeValue(const StackValue* source, const Address& dest,
                    const ValueOperand& scratch);

#define EMIT_OP(op) bool emit_##op();
    OPCODE_LIST(EMIT_OP)
#undef EMIT_OP

    // JSOP_NEG, JSOP_BITNOT
    MOZ_MUST_USE bool emitUnaryArith();

    // JSOP_BITXOR, JSOP_LSH, JSOP_ADD etc.
    MOZ_MUST_USE bool emitBinaryArith();

    // Handles JSOP_LT, JSOP_GT, and friends
    MOZ_MUST_USE bool emitCompare();

    MOZ_MUST_USE bool emitReturn();

    MOZ_MUST_USE bool emitToBoolean();
    MOZ_MUST_USE bool emitTest(bool branchIfTrue);
    MOZ_MUST_USE bool emitAndOr(bool branchIfTrue);
    MOZ_MUST_USE bool emitCall();
    MOZ_MUST_USE bool emitSpreadCall();

    MOZ_MUST_USE bool emitInitPropGetterSetter();
    MOZ_MUST_USE bool emitInitElemGetterSetter();

    MOZ_MUST_USE bool emitFormalArgAccess(uint32_t arg, bool get);

    MOZ_MUST_USE bool emitThrowConstAssignment();
    MOZ_MUST_USE bool emitUninitializedLexicalCheck(const ValueOperand& val);

    MOZ_MUST_USE bool emitIsMagicValue();

    MOZ_MUST_USE bool addPCMappingEntry(bool addIndexEntry);

    MOZ_MUST_USE bool addYieldAndAwaitOffset();

    void getEnvironmentCoordinateObject(Register reg);
    Address getEnvironmentCoordinateAddressFromObject(Register objReg, Register reg);
    Address getEnvironmentCoordinateAddress(Register reg);

    void getThisEnvironmentCallee(Register reg);
};

extern const VMFunction NewArrayCopyOnWriteInfo;
extern const VMFunction ImplicitThisInfo;

} // namespace jit
} // namespace js

#endif /* jit_BaselineCompiler_h */
