/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JavaScript "portable baseline interpreter": an interpreter that is
 * capable of running ICs, but without any native code.
 *
 * See the [SMDOC] in vm/PortableBaselineInterpret.h for a high-level
 * overview.
 */

#include "vm/PortableBaselineInterpret.h"

#include "mozilla/Maybe.h"
#include <algorithm>

#include "fdlibm.h"
#include "jsapi.h"

#include "builtin/DataViewObject.h"
#include "builtin/MapObject.h"
#include "builtin/Object.h"
#include "builtin/RegExp.h"
#include "builtin/String.h"
#include "debugger/DebugAPI.h"
#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRReader.h"
#include "jit/JitFrames.h"
#include "jit/JitScript.h"
#include "jit/JSJitFrameIter.h"
#include "jit/VMFunctions.h"
#include "proxy/DeadObjectProxy.h"
#include "proxy/DOMProxy.h"
#include "util/Unicode.h"
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/DateObject.h"
#include "vm/EnvironmentObject.h"
#include "vm/EqualityOperations.h"
#include "vm/GeneratorObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JitActivation.h"
#include "vm/JSObject.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/PlainObject.h"
#include "vm/Shape.h"
#include "vm/TypeofEqOperand.h"  // TypeofEqOperand
#include "vm/WrapperObject.h"

#include "debugger/DebugAPI-inl.h"
#include "jit/BaselineFrame-inl.h"
#include "jit/JitScript-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/JSScript-inl.h"
#include "vm/PlainObject-inl.h"

namespace js {
namespace pbl {

using namespace js::jit;

/*
 * Debugging: enable `TRACE_INTERP` for an extremely detailed dump of
 * what PBL is doing at every opcode step.
 */

// #define TRACE_INTERP

#ifdef TRACE_INTERP
#  define TRACE_PRINTF(...) \
    do {                    \
      printf(__VA_ARGS__);  \
      fflush(stdout);       \
    } while (0)
#else
#  define TRACE_PRINTF(...) \
    do {                    \
    } while (0)
#endif

#define PBL_HYBRID_ICS_DEFAULT true

// Whether we are using the "hybrid" strategy for ICs (see the [SMDOC]
// in PortableBaselineInterpret.h for more). This is currently a
// constant, but may become configurable in the future.
static const bool kHybridICsInterp = PBL_HYBRID_ICS_DEFAULT;

// Whether to compile interpreter dispatch loops using computed gotos
// or direct switches.
#if !defined(__wasi__) && !defined(TRACE_INTERP)
#  define ENABLE_COMPUTED_GOTO_DISPATCH
#endif

// Whether to compile in interrupt checks in the main interpreter loop.
#ifndef __wasi__
// On WASI, with a single thread, there is no possibility for an
// interrupt to come asynchronously.
#  define ENABLE_INTERRUPT_CHECKS
#endif

// Whether to compile in coverage counting in the main interpreter loop.
#ifndef __wasi__
#  define ENABLE_COVERAGE
#endif

/*
 * -----------------------------------------------
 * Stack handling
 * -----------------------------------------------
 */

// Large enough for an exit frame.
static const size_t kStackMargin = 1024;

/*
 * A 64-bit value on the auxiliary stack. May either be a raw uint64_t
 * or a `Value` (JS NaN-boxed value).
 */
struct StackVal {
  uint64_t value;

  explicit StackVal(uint64_t v) : value(v) {}
  explicit StackVal(const Value& v) : value(v.asRawBits()) {}

  uint64_t asUInt64() const { return value; }
  Value asValue() const { return Value::fromRawBits(value); }
};

/*
 * A native-pointer-sized value on the auxiliary stack. This is
 * separate from the above because we support running on 32-bit
 * systems as well! May either be a `void*` (or cast to a
 * `CalleeToken`, which is a typedef for a `void*`), or a `uint32_t`,
 * which always fits in a native pointer width on our supported
 * platforms. (See static_assert below.)
 */
struct StackValNative {
  static_assert(sizeof(uintptr_t) >= sizeof(uint32_t),
                "Must be at least a 32-bit system to use PBL.");

  uintptr_t value;

  explicit StackValNative(void* v) : value(reinterpret_cast<uintptr_t>(v)) {}
  explicit StackValNative(uint32_t v) : value(v) {}

  void* asVoidPtr() const { return reinterpret_cast<void*>(value); }
  CalleeToken asCalleeToken() const {
    return reinterpret_cast<CalleeToken>(value);
  }
};

// Assert that the stack alignment is no more than the size of a
// StackValNative -- we rely on this when setting up call frames.
static_assert(JitStackAlignment <= sizeof(StackValNative));

#define PUSH(val) *--sp = (val)
#define POP() (*sp++)
#define POPN(n) sp += (n)

#define PUSHNATIVE(val)                                               \
  do {                                                                \
    StackValNative* nativeSP = reinterpret_cast<StackValNative*>(sp); \
    *--nativeSP = (val);                                              \
    sp = reinterpret_cast<StackVal*>(nativeSP);                       \
  } while (0)
#define POPNNATIVE(n) \
  sp = reinterpret_cast<StackVal*>(reinterpret_cast<StackValNative*>(sp) + (n))

/*
 * Helper class to manage the auxiliary stack and push/pop frames.
 */
struct Stack {
  StackVal* fp;
  StackVal* base;
  StackVal* top;
  StackVal* unwindingSP;
  StackVal* unwindingFP;

  explicit Stack(PortableBaselineStack& pbs)
      : fp(reinterpret_cast<StackVal*>(pbs.top)),
        base(reinterpret_cast<StackVal*>(pbs.base)),
        top(reinterpret_cast<StackVal*>(pbs.top)),
        unwindingSP(nullptr),
        unwindingFP(nullptr) {}

  MOZ_ALWAYS_INLINE bool check(StackVal* sp, size_t size, bool margin = true) {
    return reinterpret_cast<uintptr_t>(base) + size +
               (margin ? kStackMargin : 0) <=
           reinterpret_cast<uintptr_t>(sp);
  }

  [[nodiscard]] MOZ_ALWAYS_INLINE StackVal* allocate(StackVal* sp,
                                                     size_t size) {
    if (!check(sp, size, false)) {
      return nullptr;
    }
    sp = reinterpret_cast<StackVal*>(reinterpret_cast<uintptr_t>(sp) - size);
    return sp;
  }

  uint32_t frameSize(StackVal* sp, BaselineFrame* curFrame) const {
    return sizeof(StackVal) * (reinterpret_cast<StackVal*>(fp) - sp);
  }

  [[nodiscard]] MOZ_ALWAYS_INLINE BaselineFrame* pushFrame(StackVal* sp,
                                                           JSContext* cx,
                                                           JSObject* envChain) {
    TRACE_PRINTF("pushFrame: sp = %p fp = %p\n", sp, fp);
    if (sp == base) {
      return nullptr;
    }
    PUSHNATIVE(StackValNative(fp));
    fp = sp;
    TRACE_PRINTF("pushFrame: new fp = %p\n", fp);

    BaselineFrame* frame =
        reinterpret_cast<BaselineFrame*>(allocate(sp, BaselineFrame::Size()));
    if (!frame) {
      return nullptr;
    }

    frame->setFlags(BaselineFrame::Flags::RUNNING_IN_INTERPRETER);
    frame->setEnvironmentChain(envChain);
    JSScript* script = frame->script();
    frame->setICScript(script->jitScript()->icScript());
    frame->setInterpreterFieldsForPrologue(script);
#ifdef DEBUG
    frame->setDebugFrameSize(0);
#endif
    return frame;
  }

  StackVal* popFrame() {
    StackVal* newTOS =
        reinterpret_cast<StackVal*>(reinterpret_cast<StackValNative*>(fp) + 1);
    fp = reinterpret_cast<StackVal*>(
        reinterpret_cast<StackValNative*>(fp)->asVoidPtr());
    MOZ_ASSERT(fp);
    TRACE_PRINTF("popFrame: fp = %p\n", fp);
    return newTOS;
  }

  void setFrameSize(StackVal* sp, BaselineFrame* prevFrame) {
#ifdef DEBUG
    MOZ_ASSERT(fp != nullptr);
    uintptr_t frameSize =
        reinterpret_cast<uintptr_t>(fp) - reinterpret_cast<uintptr_t>(sp);
    MOZ_ASSERT(reinterpret_cast<uintptr_t>(fp) >=
               reinterpret_cast<uintptr_t>(sp));
    TRACE_PRINTF("pushExitFrame: fp = %p cur() = %p -> frameSize = %d\n", fp,
                 sp, int(frameSize));
    MOZ_ASSERT(frameSize >= BaselineFrame::Size());
    prevFrame->setDebugFrameSize(frameSize);
#endif
  }

  [[nodiscard]] MOZ_ALWAYS_INLINE StackVal* pushExitFrame(
      StackVal* sp, BaselineFrame* prevFrame) {
    uint8_t* prevFP =
        reinterpret_cast<uint8_t*>(prevFrame) + BaselineFrame::Size();
    TRACE_PRINTF(
        "pushExitFrame: prevFrame = %p sp = %p BaselineFrame::Size() = %d -> "
        "computed prevFP = %p actual fp = %p\n",
        prevFrame, sp, int(BaselineFrame::Size()), prevFP, fp);
    MOZ_ASSERT(reinterpret_cast<StackVal*>(prevFP) == fp);
    setFrameSize(sp, prevFrame);

    if (!check(sp, sizeof(StackVal) * 4, false)) {
      return nullptr;
    }

    PUSHNATIVE(StackValNative(
        MakeFrameDescriptorForJitCall(FrameType::BaselineJS, 0)));
    PUSHNATIVE(StackValNative(nullptr));  // fake return address.
    PUSHNATIVE(StackValNative(prevFP));
    StackVal* exitFP = sp;
    fp = exitFP;
    TRACE_PRINTF(" -> fp = %p\n", fp);
    PUSHNATIVE(StackValNative(uint32_t(ExitFrameType::Bare)));
    return exitFP;
  }

  void popExitFrame(StackVal* fp) {
    StackVal* prevFP = reinterpret_cast<StackVal*>(
        reinterpret_cast<StackValNative*>(fp)->asVoidPtr());
    MOZ_ASSERT(prevFP);
    this->fp = prevFP;
    TRACE_PRINTF("popExitFrame: fp -> %p\n", fp);
  }

  BaselineFrame* frameFromFP() {
    return reinterpret_cast<BaselineFrame*>(reinterpret_cast<uintptr_t>(fp) -
                                            BaselineFrame::Size());
  }

  static HandleValue handle(StackVal* sp) {
    return HandleValue::fromMarkedLocation(reinterpret_cast<Value*>(sp));
  }
  static MutableHandleValue handleMut(StackVal* sp) {
    return MutableHandleValue::fromMarkedLocation(reinterpret_cast<Value*>(sp));
  }
};

/*
 * -----------------------------------------------
 * Interpreter state
 * -----------------------------------------------
 */

struct ICRegs {
  static const int kMaxICVals = 16;
  // Values can be split across two OR'd halves: unboxed bits and
  // tags.  We mostly rely on the CacheIRWriter/Reader typed OperandId
  // system to ensure "type safety" in CacheIR w.r.t. unboxing: the
  // existence of an ObjOperandId implies that the value is unboxed,
  // so `icVals` contains a pointer (reinterpret-casted to a
  // `uint64_t`) and `icTags` contains the tag bits. An operator that
  // requires a tagged Value can OR the two together (this corresponds
  // to `useValueRegister` rather than `useRegister` in the native
  // baseline compiler).
  uint64_t icVals[kMaxICVals];
  uint64_t icTags[kMaxICVals];  // Shifted tags.
  int extraArgs;
};

struct State {
  RootedValue value0;
  RootedValue value1;
  RootedValue value2;
  RootedValue value3;
  RootedValue res;
  RootedObject obj0;
  RootedObject obj1;
  RootedObject obj2;
  RootedString str0;
  RootedString str1;
  RootedString str2;
  RootedScript script0;
  Rooted<PropertyName*> name0;
  Rooted<jsid> id0;
  Rooted<JSAtom*> atom0;
  RootedFunction fun0;
  Rooted<Scope*> scope0;

  explicit State(JSContext* cx)
      : value0(cx),
        value1(cx),
        value2(cx),
        value3(cx),
        res(cx),
        obj0(cx),
        obj1(cx),
        obj2(cx),
        str0(cx),
        str1(cx),
        str2(cx),
        script0(cx),
        name0(cx),
        id0(cx),
        atom0(cx),
        fun0(cx),
        scope0(cx) {}
};

/*
 * -----------------------------------------------
 * RAII helpers for pushing exit frames.
 *
 * (See [SMDOC] in PortableBaselineInterpret.h for more.)
 * -----------------------------------------------
 */

class VMFrameManager {
  JSContext* cx;
  BaselineFrame* frame;
  friend class VMFrame;

 public:
  VMFrameManager(JSContext*& cx_, BaselineFrame* frame_)
      : cx(cx_), frame(frame_) {
    // Once the manager exists, we need to create an exit frame to
    // have access to the cx (unless the caller promises it is not
    // calling into the rest of the runtime).
    cx_ = nullptr;
  }

  void switchToFrame(BaselineFrame* frame) { this->frame = frame; }

  // Provides the JSContext, but *only* if no calls into the rest of
  // the runtime (that may invoke a GC or stack walk) occur. Avoids
  // the overhead of pushing an exit frame.
  JSContext* cxForLocalUseOnly() const { return cx; }
};

class VMFrame {
  JSContext* cx;
  Stack& stack;
  StackVal* exitFP;
  void* prevSavedStack;

 public:
  VMFrame(VMFrameManager& mgr, Stack& stack_, StackVal* sp)
      : cx(mgr.cx), stack(stack_) {
    exitFP = stack.pushExitFrame(sp, mgr.frame);
    if (!exitFP) {
      return;
    }
    cx->activation()->asJit()->setJSExitFP(reinterpret_cast<uint8_t*>(exitFP));
    prevSavedStack = cx->portableBaselineStack().top;
    cx->portableBaselineStack().top = reinterpret_cast<void*>(spBelowFrame());
  }

  StackVal* spBelowFrame() {
    return reinterpret_cast<StackVal*>(reinterpret_cast<uintptr_t>(exitFP) -
                                       sizeof(StackValNative));
  }

  ~VMFrame() {
    stack.popExitFrame(exitFP);
    cx->portableBaselineStack().top = prevSavedStack;
  }

  JSContext* getCx() const { return cx; }
  operator JSContext*() const { return cx; }

  bool success() const { return exitFP != nullptr; }
};

#define PUSH_EXIT_FRAME_OR_RET(value, init_sp)                  \
  VMFrame cx(ctx.frameMgr, ctx.stack, init_sp);                 \
  if (!cx.success()) {                                          \
    return value;                                               \
  }                                                             \
  StackVal* sp = cx.spBelowFrame(); /* shadow the definition */ \
  (void)sp;                         /* avoid unused-variable warnings */

#define PUSH_IC_FRAME()         \
  ctx.error = PBIResult::Error; \
  PUSH_EXIT_FRAME_OR_RET(IC_ERROR_SENTINEL(), ctx.sp())
#define PUSH_FALLBACK_IC_FRAME() \
  ctx.error = PBIResult::Error;  \
  PUSH_EXIT_FRAME_OR_RET(IC_ERROR_SENTINEL(), sp)
#define PUSH_EXIT_FRAME()      \
  frame->interpreterPC() = pc; \
  SYNCSP();                    \
  PUSH_EXIT_FRAME_OR_RET(PBIResult::Error, sp)

/*
 * -----------------------------------------------
 * IC Interpreter
 * -----------------------------------------------
 */

// Bundled state for passing to ICs, in order to reduce the number of
// arguments and hence make the call more ABI-efficient. (On some
// platforms, e.g. Wasm on Wasmtime on x86-64, we have as few as four
// register arguments available before args go through the stack.)
struct ICCtx {
  BaselineFrame* frame;
  VMFrameManager frameMgr;
  State& state;
  ICRegs icregs;
  Stack& stack;
  StackVal* sp_;
  PBIResult error;
  uint64_t arg2;

  ICCtx(JSContext* cx, BaselineFrame* frame_, State& state_, Stack& stack_)
      : frame(frame_),
        frameMgr(cx, frame_),
        state(state_),
        icregs(),
        stack(stack_),
        sp_(nullptr),
        error(PBIResult::Ok),
        arg2(0) {}

  StackVal* sp() { return sp_; }
};

#define IC_ERROR_SENTINEL() (JS::MagicValue(JS_GENERIC_MAGIC).asRawBits())

// Universal signature for an IC stub function.
typedef uint64_t (*ICStubFunc)(uint64_t arg0, uint64_t arg1, ICStub* stub,
                               ICCtx& ctx);

#define PBL_CALL_IC(jitcode, ctx, stubvalue, result, arg0, arg1, arg2value, \
                    hasarg2)                                                \
  do {                                                                      \
    ctx.arg2 = arg2value;                                                   \
    ICStubFunc func = reinterpret_cast<ICStubFunc>(jitcode);                \
    result = func(arg0, arg1, stubvalue, ctx);                              \
  } while (0)

typedef PBIResult (*PBIFunc)(JSContext* cx_, State& state, Stack& stack,
                             StackVal* sp, JSObject* envChain, Value* ret,
                             jsbytecode* pc, ImmutableScriptData* isd,
                             jsbytecode* restartEntryPC,
                             BaselineFrame* restartFrame,
                             StackVal* restartEntryFrame,
                             PBIResult restartCode);

static uint64_t CallNextIC(uint64_t arg0, uint64_t arg1, ICStub* stub,
                           ICCtx& ctx);

static double DoubleMinMax(bool isMax, double first, double second) {
  if (std::isnan(first) || std::isnan(second)) {
    return JS::GenericNaN();
  } else if (first == 0 && second == 0) {
    // -0 and 0 compare as equal, but we have to distinguish
    // them here: min(-0, 0) = -0, max(-0, 0) = 0.
    bool firstPos = !std::signbit(first);
    bool secondPos = !std::signbit(second);
    bool sign = isMax ? (firstPos || secondPos) : (firstPos && secondPos);
    return sign ? 0.0 : -0.0;
  } else {
    return isMax ? ((first >= second) ? first : second)
                 : ((first <= second) ? first : second);
  }
}

// Interpreter for CacheIR.
uint64_t ICInterpretOps(uint64_t arg0, uint64_t arg1, ICStub* stub,
                        ICCtx& ctx) {
  {
#define DECLARE_CACHEOP_CASE(name) __label__ cacheop_##name

#ifdef ENABLE_COMPUTED_GOTO_DISPATCH

#  define CACHEOP_CASE(name) cacheop_##name : CACHEOP_TRACE(name)
#  define CACHEOP_CASE_FALLTHROUGH(name) CACHEOP_CASE(name)

#  define DISPATCH_CACHEOP()          \
    cacheop = cacheIRReader.readOp(); \
    goto* addresses[long(cacheop)];

#else  // ENABLE_COMPUTED_GOTO_DISPATCH

#  define CACHEOP_CASE(name) \
    case CacheOp::name:      \
      cacheop_##name : CACHEOP_TRACE(name)
#  define CACHEOP_CASE_FALLTHROUGH(name) \
    [[fallthrough]];                     \
    CACHEOP_CASE(name)

#  define DISPATCH_CACHEOP()          \
    cacheop = cacheIRReader.readOp(); \
    goto dispatch;

#endif  // !ENABLE_COMPUTED_GOTO_DISPATCH

#define READ_REG(index) ctx.icregs.icVals[(index)]
#define READ_VALUE_REG(index) \
  Value::fromRawBits(ctx.icregs.icVals[(index)] | ctx.icregs.icTags[(index)])
#define WRITE_REG(index, value, tag)                                           \
  do {                                                                         \
    ctx.icregs.icVals[(index)] = (value);                                      \
    ctx.icregs.icTags[(index)] = uint64_t(JSVAL_TAG_##tag) << JSVAL_TAG_SHIFT; \
  } while (0)
#define WRITE_VALUE_REG(index, value)                 \
  do {                                                \
    ctx.icregs.icVals[(index)] = (value).asRawBits(); \
    ctx.icregs.icTags[(index)] = 0;                   \
  } while (0)

    DECLARE_CACHEOP_CASE(ReturnFromIC);
    DECLARE_CACHEOP_CASE(GuardToObject);
    DECLARE_CACHEOP_CASE(GuardIsNullOrUndefined);
    DECLARE_CACHEOP_CASE(GuardIsNull);
    DECLARE_CACHEOP_CASE(GuardIsUndefined);
    DECLARE_CACHEOP_CASE(GuardIsNotUninitializedLexical);
    DECLARE_CACHEOP_CASE(GuardToBoolean);
    DECLARE_CACHEOP_CASE(GuardToString);
    DECLARE_CACHEOP_CASE(GuardToSymbol);
    DECLARE_CACHEOP_CASE(GuardToBigInt);
    DECLARE_CACHEOP_CASE(GuardIsNumber);
    DECLARE_CACHEOP_CASE(GuardToInt32);
    DECLARE_CACHEOP_CASE(GuardToNonGCThing);
    DECLARE_CACHEOP_CASE(GuardBooleanToInt32);
    DECLARE_CACHEOP_CASE(GuardToInt32Index);
    DECLARE_CACHEOP_CASE(Int32ToIntPtr);
    DECLARE_CACHEOP_CASE(GuardToInt32ModUint32);
    DECLARE_CACHEOP_CASE(GuardNonDoubleType);
    DECLARE_CACHEOP_CASE(GuardShape);
    DECLARE_CACHEOP_CASE(GuardFuse);
    DECLARE_CACHEOP_CASE(GuardProto);
    DECLARE_CACHEOP_CASE(GuardNullProto);
    DECLARE_CACHEOP_CASE(GuardClass);
    DECLARE_CACHEOP_CASE(GuardAnyClass);
    DECLARE_CACHEOP_CASE(GuardGlobalGeneration);
    DECLARE_CACHEOP_CASE(HasClassResult);
    DECLARE_CACHEOP_CASE(GuardCompartment);
    DECLARE_CACHEOP_CASE(GuardIsExtensible);
    DECLARE_CACHEOP_CASE(GuardIsNativeObject);
    DECLARE_CACHEOP_CASE(GuardIsProxy);
    DECLARE_CACHEOP_CASE(GuardIsNotProxy);
    DECLARE_CACHEOP_CASE(GuardIsNotArrayBufferMaybeShared);
    DECLARE_CACHEOP_CASE(GuardIsTypedArray);
    DECLARE_CACHEOP_CASE(GuardHasProxyHandler);
    DECLARE_CACHEOP_CASE(GuardIsNotDOMProxy);
    DECLARE_CACHEOP_CASE(GuardSpecificObject);
    DECLARE_CACHEOP_CASE(GuardObjectIdentity);
    DECLARE_CACHEOP_CASE(GuardSpecificFunction);
    DECLARE_CACHEOP_CASE(GuardFunctionScript);
    DECLARE_CACHEOP_CASE(GuardSpecificAtom);
    DECLARE_CACHEOP_CASE(GuardSpecificSymbol);
    DECLARE_CACHEOP_CASE(GuardSpecificInt32);
    DECLARE_CACHEOP_CASE(GuardNoDenseElements);
    DECLARE_CACHEOP_CASE(GuardStringToIndex);
    DECLARE_CACHEOP_CASE(GuardStringToInt32);
    DECLARE_CACHEOP_CASE(GuardStringToNumber);
    DECLARE_CACHEOP_CASE(BooleanToNumber);
    DECLARE_CACHEOP_CASE(GuardHasGetterSetter);
    DECLARE_CACHEOP_CASE(GuardInt32IsNonNegative);
    DECLARE_CACHEOP_CASE(GuardDynamicSlotIsSpecificObject);
    DECLARE_CACHEOP_CASE(GuardDynamicSlotIsNotObject);
    DECLARE_CACHEOP_CASE(GuardFixedSlotValue);
    DECLARE_CACHEOP_CASE(GuardDynamicSlotValue);
    DECLARE_CACHEOP_CASE(LoadFixedSlot);
    DECLARE_CACHEOP_CASE(LoadDynamicSlot);
    DECLARE_CACHEOP_CASE(GuardNoAllocationMetadataBuilder);
    DECLARE_CACHEOP_CASE(GuardFunctionHasJitEntry);
    DECLARE_CACHEOP_CASE(GuardFunctionHasNoJitEntry);
    DECLARE_CACHEOP_CASE(GuardFunctionIsNonBuiltinCtor);
    DECLARE_CACHEOP_CASE(GuardFunctionIsConstructor);
    DECLARE_CACHEOP_CASE(GuardNotClassConstructor);
    DECLARE_CACHEOP_CASE(GuardArrayIsPacked);
    DECLARE_CACHEOP_CASE(GuardArgumentsObjectFlags);
    DECLARE_CACHEOP_CASE(LoadObject);
    DECLARE_CACHEOP_CASE(LoadProtoObject);
    DECLARE_CACHEOP_CASE(LoadProto);
    DECLARE_CACHEOP_CASE(LoadEnclosingEnvironment);
    DECLARE_CACHEOP_CASE(LoadWrapperTarget);
    DECLARE_CACHEOP_CASE(LoadValueTag);
    DECLARE_CACHEOP_CASE(LoadArgumentFixedSlot);
    DECLARE_CACHEOP_CASE(LoadArgumentDynamicSlot);
    DECLARE_CACHEOP_CASE(TruncateDoubleToUInt32);
    DECLARE_CACHEOP_CASE(MegamorphicLoadSlotResult);
    DECLARE_CACHEOP_CASE(MegamorphicLoadSlotByValueResult);
    DECLARE_CACHEOP_CASE(MegamorphicSetElement);
    DECLARE_CACHEOP_CASE(StoreFixedSlot);
    DECLARE_CACHEOP_CASE(StoreDynamicSlot);
    DECLARE_CACHEOP_CASE(AddAndStoreFixedSlot);
    DECLARE_CACHEOP_CASE(AddAndStoreDynamicSlot);
    DECLARE_CACHEOP_CASE(AllocateAndStoreDynamicSlot);
    DECLARE_CACHEOP_CASE(StoreDenseElement);
    DECLARE_CACHEOP_CASE(StoreDenseElementHole);
    DECLARE_CACHEOP_CASE(ArrayPush);
    DECLARE_CACHEOP_CASE(IsObjectResult);
    DECLARE_CACHEOP_CASE(Int32MinMax);
    DECLARE_CACHEOP_CASE(StoreTypedArrayElement);
    DECLARE_CACHEOP_CASE(CallInt32ToString);
    DECLARE_CACHEOP_CASE(CallScriptedFunction);
    DECLARE_CACHEOP_CASE(CallNativeFunction);
    DECLARE_CACHEOP_CASE(MetaScriptedThisShape);
    DECLARE_CACHEOP_CASE(LoadFixedSlotResult);
    DECLARE_CACHEOP_CASE(LoadDynamicSlotResult);
    DECLARE_CACHEOP_CASE(LoadDenseElementResult);
    DECLARE_CACHEOP_CASE(LoadInt32ArrayLengthResult);
    DECLARE_CACHEOP_CASE(LoadInt32ArrayLength);
    DECLARE_CACHEOP_CASE(LoadArgumentsObjectArgResult);
    DECLARE_CACHEOP_CASE(LinearizeForCharAccess);
    DECLARE_CACHEOP_CASE(LoadStringCharResult);
    DECLARE_CACHEOP_CASE(LoadStringCharCodeResult);
    DECLARE_CACHEOP_CASE(LoadStringLengthResult);
    DECLARE_CACHEOP_CASE(LoadObjectResult);
    DECLARE_CACHEOP_CASE(LoadStringResult);
    DECLARE_CACHEOP_CASE(LoadSymbolResult);
    DECLARE_CACHEOP_CASE(LoadInt32Result);
    DECLARE_CACHEOP_CASE(LoadDoubleResult);
    DECLARE_CACHEOP_CASE(LoadBigIntResult);
    DECLARE_CACHEOP_CASE(LoadBooleanResult);
    DECLARE_CACHEOP_CASE(LoadInt32Constant);
    DECLARE_CACHEOP_CASE(LoadConstantStringResult);
    DECLARE_CACHEOP_CASE(Int32AddResult);
    DECLARE_CACHEOP_CASE(Int32SubResult);
    DECLARE_CACHEOP_CASE(Int32MulResult);
    DECLARE_CACHEOP_CASE(Int32DivResult);
    DECLARE_CACHEOP_CASE(Int32ModResult);
    DECLARE_CACHEOP_CASE(Int32BitOrResult);
    DECLARE_CACHEOP_CASE(Int32BitXorResult);
    DECLARE_CACHEOP_CASE(Int32BitAndResult);
    DECLARE_CACHEOP_CASE(Int32PowResult);
    DECLARE_CACHEOP_CASE(Int32IncResult);
    DECLARE_CACHEOP_CASE(LoadInt32TruthyResult);
    DECLARE_CACHEOP_CASE(LoadStringTruthyResult);
    DECLARE_CACHEOP_CASE(LoadObjectTruthyResult);
    DECLARE_CACHEOP_CASE(LoadValueResult);
    DECLARE_CACHEOP_CASE(LoadOperandResult);
    DECLARE_CACHEOP_CASE(ConcatStringsResult);
    DECLARE_CACHEOP_CASE(CompareStringResult);
    DECLARE_CACHEOP_CASE(CompareInt32Result);
    DECLARE_CACHEOP_CASE(CompareNullUndefinedResult);
    DECLARE_CACHEOP_CASE(AssertPropertyLookup);
    DECLARE_CACHEOP_CASE(GuardIsFixedLengthTypedArray);
    DECLARE_CACHEOP_CASE(GuardIndexIsNotDenseElement);
    DECLARE_CACHEOP_CASE(LoadFixedSlotTypedResult);
    DECLARE_CACHEOP_CASE(LoadDenseElementHoleResult);
    DECLARE_CACHEOP_CASE(LoadDenseElementExistsResult);
    DECLARE_CACHEOP_CASE(LoadTypedArrayElementExistsResult);
    DECLARE_CACHEOP_CASE(LoadDenseElementHoleExistsResult);
    DECLARE_CACHEOP_CASE(LoadTypedArrayElementResult);
    DECLARE_CACHEOP_CASE(RegExpFlagResult);
    DECLARE_CACHEOP_CASE(GuardNumberToIntPtrIndex);
    DECLARE_CACHEOP_CASE(CallRegExpMatcherResult);
    DECLARE_CACHEOP_CASE(CallRegExpSearcherResult);
    DECLARE_CACHEOP_CASE(RegExpSearcherLastLimitResult);
    DECLARE_CACHEOP_CASE(RegExpHasCaptureGroupsResult);
    DECLARE_CACHEOP_CASE(RegExpBuiltinExecMatchResult);
    DECLARE_CACHEOP_CASE(RegExpBuiltinExecTestResult);
    DECLARE_CACHEOP_CASE(CallSubstringKernelResult);
    DECLARE_CACHEOP_CASE(StringReplaceStringResult);
    DECLARE_CACHEOP_CASE(StringSplitStringResult);
    DECLARE_CACHEOP_CASE(GetFirstDollarIndexResult);
    DECLARE_CACHEOP_CASE(StringToAtom);
    DECLARE_CACHEOP_CASE(GuardTagNotEqual);
    DECLARE_CACHEOP_CASE(IdToStringOrSymbol);
    DECLARE_CACHEOP_CASE(MegamorphicStoreSlot);
    DECLARE_CACHEOP_CASE(MegamorphicHasPropResult);
    DECLARE_CACHEOP_CASE(ObjectToIteratorResult);
    DECLARE_CACHEOP_CASE(ArrayJoinResult);
    DECLARE_CACHEOP_CASE(ObjectKeysResult);
    DECLARE_CACHEOP_CASE(PackedArrayPopResult);
    DECLARE_CACHEOP_CASE(PackedArrayShiftResult);
    DECLARE_CACHEOP_CASE(PackedArraySliceResult);
    DECLARE_CACHEOP_CASE(IsArrayResult);
    DECLARE_CACHEOP_CASE(IsPackedArrayResult);
    DECLARE_CACHEOP_CASE(IsCallableResult);
    DECLARE_CACHEOP_CASE(IsConstructorResult);
    DECLARE_CACHEOP_CASE(IsCrossRealmArrayConstructorResult);
    DECLARE_CACHEOP_CASE(IsTypedArrayResult);
    DECLARE_CACHEOP_CASE(IsTypedArrayConstructorResult);
    DECLARE_CACHEOP_CASE(ArrayBufferViewByteOffsetInt32Result);
    DECLARE_CACHEOP_CASE(ArrayBufferViewByteOffsetDoubleResult);
    DECLARE_CACHEOP_CASE(TypedArrayByteLengthInt32Result);
    DECLARE_CACHEOP_CASE(TypedArrayByteLengthDoubleResult);
    DECLARE_CACHEOP_CASE(TypedArrayElementSizeResult);
    DECLARE_CACHEOP_CASE(NewStringIteratorResult);
    DECLARE_CACHEOP_CASE(NewRegExpStringIteratorResult);
    DECLARE_CACHEOP_CASE(ObjectCreateResult);
    DECLARE_CACHEOP_CASE(NewArrayFromLengthResult);
    DECLARE_CACHEOP_CASE(NewTypedArrayFromArrayBufferResult);
    DECLARE_CACHEOP_CASE(NewTypedArrayFromArrayResult);
    DECLARE_CACHEOP_CASE(NewTypedArrayFromLengthResult);
    DECLARE_CACHEOP_CASE(StringFromCharCodeResult);
    DECLARE_CACHEOP_CASE(StringFromCodePointResult);
    DECLARE_CACHEOP_CASE(StringIncludesResult);
    DECLARE_CACHEOP_CASE(StringIndexOfResult);
    DECLARE_CACHEOP_CASE(StringLastIndexOfResult);
    DECLARE_CACHEOP_CASE(StringStartsWithResult);
    DECLARE_CACHEOP_CASE(StringEndsWithResult);
    DECLARE_CACHEOP_CASE(StringToLowerCaseResult);
    DECLARE_CACHEOP_CASE(StringToUpperCaseResult);
    DECLARE_CACHEOP_CASE(StringTrimResult);
    DECLARE_CACHEOP_CASE(StringTrimStartResult);
    DECLARE_CACHEOP_CASE(StringTrimEndResult);
    DECLARE_CACHEOP_CASE(MathAbsInt32Result);
    DECLARE_CACHEOP_CASE(MathAbsNumberResult);
    DECLARE_CACHEOP_CASE(MathClz32Result);
    DECLARE_CACHEOP_CASE(MathSignInt32Result);
    DECLARE_CACHEOP_CASE(MathSignNumberResult);
    DECLARE_CACHEOP_CASE(MathSignNumberToInt32Result);
    DECLARE_CACHEOP_CASE(MathImulResult);
    DECLARE_CACHEOP_CASE(MathSqrtNumberResult);
    DECLARE_CACHEOP_CASE(MathFRoundNumberResult);
    DECLARE_CACHEOP_CASE(MathRandomResult);
    DECLARE_CACHEOP_CASE(MathHypot2NumberResult);
    DECLARE_CACHEOP_CASE(MathHypot3NumberResult);
    DECLARE_CACHEOP_CASE(MathHypot4NumberResult);
    DECLARE_CACHEOP_CASE(MathAtan2NumberResult);
    DECLARE_CACHEOP_CASE(MathFloorNumberResult);
    DECLARE_CACHEOP_CASE(MathCeilNumberResult);
    DECLARE_CACHEOP_CASE(MathTruncNumberResult);
    DECLARE_CACHEOP_CASE(MathCeilToInt32Result);
    DECLARE_CACHEOP_CASE(MathFloorToInt32Result);
    DECLARE_CACHEOP_CASE(MathTruncToInt32Result);
    DECLARE_CACHEOP_CASE(MathRoundToInt32Result);
    DECLARE_CACHEOP_CASE(NumberMinMax);
    DECLARE_CACHEOP_CASE(Int32MinMaxArrayResult);
    DECLARE_CACHEOP_CASE(NumberMinMaxArrayResult);
    DECLARE_CACHEOP_CASE(MathFunctionNumberResult);
    DECLARE_CACHEOP_CASE(NumberParseIntResult);
    DECLARE_CACHEOP_CASE(DoubleParseIntResult);
    DECLARE_CACHEOP_CASE(ObjectToStringResult);
    DECLARE_CACHEOP_CASE(CallNativeSetter);
    DECLARE_CACHEOP_CASE(CallSetArrayLength);
    DECLARE_CACHEOP_CASE(CallNumberToString);
    DECLARE_CACHEOP_CASE(Int32ToStringWithBaseResult);
    DECLARE_CACHEOP_CASE(BooleanToString);
    DECLARE_CACHEOP_CASE(BindFunctionResult);
    DECLARE_CACHEOP_CASE(SpecializedBindFunctionResult);
    DECLARE_CACHEOP_CASE(CallGetSparseElementResult);
    DECLARE_CACHEOP_CASE(LoadArgumentsObjectLengthResult);
    DECLARE_CACHEOP_CASE(LoadArgumentsObjectLength);
    DECLARE_CACHEOP_CASE(LoadBoundFunctionNumArgs);
    DECLARE_CACHEOP_CASE(LoadBoundFunctionTarget);
    DECLARE_CACHEOP_CASE(LoadArrayBufferByteLengthInt32Result);
    DECLARE_CACHEOP_CASE(LoadArrayBufferByteLengthDoubleResult);
    DECLARE_CACHEOP_CASE(LinearizeForCodePointAccess);
    DECLARE_CACHEOP_CASE(LoadArrayBufferViewLengthInt32Result);
    DECLARE_CACHEOP_CASE(LoadArrayBufferViewLengthDoubleResult);
    DECLARE_CACHEOP_CASE(LoadStringAtResult);
    DECLARE_CACHEOP_CASE(LoadStringCodePointResult);
    DECLARE_CACHEOP_CASE(CallNativeGetterResult);
    DECLARE_CACHEOP_CASE(LoadUndefinedResult);
    DECLARE_CACHEOP_CASE(LoadDoubleConstant);
    DECLARE_CACHEOP_CASE(LoadBooleanConstant);
    DECLARE_CACHEOP_CASE(LoadUndefined);
    DECLARE_CACHEOP_CASE(LoadConstantString);
    DECLARE_CACHEOP_CASE(LoadInstanceOfObjectResult);
    DECLARE_CACHEOP_CASE(LoadTypeOfObjectResult);
    DECLARE_CACHEOP_CASE(DoubleAddResult);
    DECLARE_CACHEOP_CASE(DoubleSubResult);
    DECLARE_CACHEOP_CASE(DoubleMulResult);
    DECLARE_CACHEOP_CASE(DoubleDivResult);
    DECLARE_CACHEOP_CASE(DoubleModResult);
    DECLARE_CACHEOP_CASE(DoublePowResult);
    DECLARE_CACHEOP_CASE(Int32LeftShiftResult);
    DECLARE_CACHEOP_CASE(Int32RightShiftResult);
    DECLARE_CACHEOP_CASE(Int32URightShiftResult);
    DECLARE_CACHEOP_CASE(Int32NotResult);
    DECLARE_CACHEOP_CASE(LoadDoubleTruthyResult);
    DECLARE_CACHEOP_CASE(NewPlainObjectResult);
    DECLARE_CACHEOP_CASE(NewArrayObjectResult);
    DECLARE_CACHEOP_CASE(CompareObjectResult);
    DECLARE_CACHEOP_CASE(CompareSymbolResult);
    DECLARE_CACHEOP_CASE(CompareDoubleResult);
    DECLARE_CACHEOP_CASE(IndirectTruncateInt32Result);
    DECLARE_CACHEOP_CASE(CallScriptedSetter);
    DECLARE_CACHEOP_CASE(CallBoundScriptedFunction);
    DECLARE_CACHEOP_CASE(CallScriptedGetterResult);

    // Define the computed-goto table regardless of dispatch strategy so
    // we don't get unused-label errors. (We need some of the labels
    // even without this for the predict-next mechanism, so we can't
    // conditionally elide labels either.)
    static const void* const addresses[long(CacheOp::NumOpcodes)] = {
#define OP(name, ...) &&cacheop_##name,
        CACHE_IR_OPS(OP)
#undef OP
    };
    (void)addresses;

#define CACHEOP_TRACE(name) \
  TRACE_PRINTF("cacheop (frame %p stub %p): " #name "\n", ctx.frame, cstub);

#define FAIL_IC() goto next_ic;

// We set a fixed bound on the number of icVals which is smaller than what IC
// generators may use. As a result we can't evaluate an IC if it defines too
// many values. Note that we don't need to check this when reading from icVals
// because we should have bailed out before the earlier write which defined the
// same value. Similarly, we don't need to check writes to locations which we've
// just read from.
#define BOUNDSCHECK(resultId) \
  if (resultId.id() >= ICRegs::kMaxICVals) FAIL_IC();

#define PREDICT_NEXT(name)                       \
  if (cacheIRReader.peekOp() == CacheOp::name) { \
    cacheIRReader.readOp();                      \
    cacheop = CacheOp::name;                     \
    goto cacheop_##name;                         \
  }

#define PREDICT_RETURN()                                 \
  if (cacheIRReader.peekOp() == CacheOp::ReturnFromIC) { \
    TRACE_PRINTF("stub successful, predicted return\n"); \
    return retValue;                                     \
  }

    ICCacheIRStub* cstub = stub->toCacheIRStub();
    const CacheIRStubInfo* stubInfo = cstub->stubInfo();
    CacheIRReader cacheIRReader(stubInfo);
    uint64_t retValue = 0;
    CacheOp cacheop;

    WRITE_VALUE_REG(0, Value::fromRawBits(arg0));
    WRITE_VALUE_REG(1, Value::fromRawBits(arg1));
    WRITE_VALUE_REG(2, Value::fromRawBits(ctx.arg2));

    DISPATCH_CACHEOP();

#ifndef ENABLE_COMPUTED_GOTO_DISPATCH
  dispatch:
    switch (cacheop)
#endif
    {

      CACHEOP_CASE(ReturnFromIC) {
        TRACE_PRINTF("stub successful!\n");
        return retValue;
      }

      CACHEOP_CASE(GuardToObject) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        TRACE_PRINTF("GuardToObject: icVal %" PRIx64 "\n",
                     READ_REG(inputId.id()));
        if (!v.isObject()) {
          FAIL_IC();
        }
        WRITE_REG(inputId.id(), reinterpret_cast<uint64_t>(&v.toObject()),
                  OBJECT);
        PREDICT_NEXT(GuardShape);
        PREDICT_NEXT(GuardSpecificFunction);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNullOrUndefined) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isNullOrUndefined()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNull) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isNull()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsUndefined) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isUndefined()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNotUninitializedLexical) {
        ValOperandId valId = cacheIRReader.valOperandId();
        Value val = READ_VALUE_REG(valId.id());
        if (val == MagicValue(JS_UNINITIALIZED_LEXICAL)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToBoolean) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isBoolean()) {
          FAIL_IC();
        }
        WRITE_REG(inputId.id(), v.toBoolean() ? 1 : 0, BOOLEAN);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToString) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isString()) {
          FAIL_IC();
        }
        WRITE_REG(inputId.id(), reinterpret_cast<uint64_t>(v.toString()),
                  STRING);
        PREDICT_NEXT(GuardToString);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToSymbol) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isSymbol()) {
          FAIL_IC();
        }
        WRITE_REG(inputId.id(), reinterpret_cast<uint64_t>(v.toSymbol()),
                  SYMBOL);
        PREDICT_NEXT(GuardSpecificSymbol);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToBigInt) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isBigInt()) {
          FAIL_IC();
        }
        WRITE_REG(inputId.id(), reinterpret_cast<uint64_t>(v.toBigInt()),
                  BIGINT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNumber) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isNumber()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToInt32) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value v = READ_VALUE_REG(inputId.id());
        TRACE_PRINTF("GuardToInt32 (%d): icVal %" PRIx64 "\n", inputId.id(),
                     READ_REG(inputId.id()));
        if (!v.isInt32()) {
          FAIL_IC();
        }
        // N.B.: we don't need to unbox because the low 32 bits are
        // already the int32 itself, and we are careful when using
        // `Int32Operand`s to only use those bits.

        PREDICT_NEXT(GuardToInt32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToNonGCThing) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value input = READ_VALUE_REG(inputId.id());
        if (input.isGCThing()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardBooleanToInt32) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        Value v = READ_VALUE_REG(inputId.id());
        if (!v.isBoolean()) {
          FAIL_IC();
        }
        WRITE_REG(resultId.id(), v.toBoolean() ? 1 : 0, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToInt32Index) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        Value val = READ_VALUE_REG(inputId.id());
        if (val.isInt32()) {
          WRITE_REG(resultId.id(), val.toInt32(), INT32);
          DISPATCH_CACHEOP();
        } else if (val.isDouble()) {
          double doubleVal = val.toDouble();
          if (int32_t(doubleVal) == doubleVal) {
            WRITE_REG(resultId.id(), int32_t(doubleVal), INT32);
            DISPATCH_CACHEOP();
          }
        }
        FAIL_IC();
      }

      CACHEOP_CASE(Int32ToIntPtr) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        IntPtrOperandId resultId = cacheIRReader.intPtrOperandId();
        BOUNDSCHECK(resultId);
        int32_t input = int32_t(READ_REG(inputId.id()));
        // Note that this must sign-extend to pointer width:
        WRITE_REG(resultId.id(), intptr_t(input), OBJECT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardToInt32ModUint32) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        Value input = READ_VALUE_REG(inputId.id());
        if (input.isInt32()) {
          WRITE_REG(resultId.id(), input.toInt32(), INT32);
          DISPATCH_CACHEOP();
        } else if (input.isDouble()) {
          double doubleVal = input.toDouble();
          // Accept any double that fits in an int64_t but truncate the top 32
          // bits.
          if (doubleVal >= double(INT64_MIN) &&
              doubleVal <= double(INT64_MAX)) {
            WRITE_REG(resultId.id(), int64_t(doubleVal), INT32);
            DISPATCH_CACHEOP();
          }
        }
        FAIL_IC();
      }

      CACHEOP_CASE(GuardNonDoubleType) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        ValueType type = cacheIRReader.valueType();
        Value val = READ_VALUE_REG(inputId.id());
        switch (type) {
          case ValueType::String:
            if (!val.isString()) {
              FAIL_IC();
            }
            break;
          case ValueType::Symbol:
            if (!val.isSymbol()) {
              FAIL_IC();
            }
            break;
          case ValueType::BigInt:
            if (!val.isBigInt()) {
              FAIL_IC();
            }
            break;
          case ValueType::Int32:
            if (!val.isInt32()) {
              FAIL_IC();
            }
            break;
          case ValueType::Boolean:
            if (!val.isBoolean()) {
              FAIL_IC();
            }
            break;
          case ValueType::Undefined:
            if (!val.isUndefined()) {
              FAIL_IC();
            }
            break;
          case ValueType::Null:
            if (!val.isNull()) {
              FAIL_IC();
            }
            break;
          default:
            MOZ_CRASH("Unexpected type");
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardShape) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t shapeOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uintptr_t expectedShape = stubInfo->getStubRawWord(cstub, shapeOffset);
        if (reinterpret_cast<uintptr_t>(obj->shape()) != expectedShape) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFuse) {
        RealmFuses::FuseIndex fuseIndex = cacheIRReader.realmFuseIndex();
        if (!ctx.frameMgr.cxForLocalUseOnly()
                 ->realm()
                 ->realmFuses.getFuseByIndex(fuseIndex)
                 ->intact()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardProto) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t protoOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSObject* proto = reinterpret_cast<JSObject*>(
            stubInfo->getStubRawWord(cstub, protoOffset));
        if (obj->staticPrototype() != proto) {
          FAIL_IC();
        }
        PREDICT_NEXT(LoadProto);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardNullProto) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (obj->taggedProto().raw()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardClass) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        GuardClassKind kind = cacheIRReader.guardClassKind();
        JSObject* object = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        switch (kind) {
          case GuardClassKind::Array:
            if (object->getClass() != &ArrayObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::PlainObject:
            if (object->getClass() != &PlainObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::FixedLengthArrayBuffer:
            if (object->getClass() != &FixedLengthArrayBufferObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::ResizableArrayBuffer:
            if (object->getClass() != &ResizableArrayBufferObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::FixedLengthSharedArrayBuffer:
            if (object->getClass() !=
                &FixedLengthSharedArrayBufferObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::GrowableSharedArrayBuffer:
            if (object->getClass() !=
                &GrowableSharedArrayBufferObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::FixedLengthDataView:
            if (object->getClass() != &FixedLengthDataViewObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::ResizableDataView:
            if (object->getClass() != &ResizableDataViewObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::MappedArguments:
            if (object->getClass() != &MappedArgumentsObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::UnmappedArguments:
            if (object->getClass() != &UnmappedArgumentsObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::WindowProxy:
            if (object->getClass() != ctx.frameMgr.cxForLocalUseOnly()
                                          ->runtime()
                                          ->maybeWindowProxyClass()) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::JSFunction:
            if (!object->is<JSFunction>()) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::Set:
            if (object->getClass() != &SetObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::Map:
            if (object->getClass() != &MapObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::BoundFunction:
            if (object->getClass() != &BoundFunctionObject::class_) {
              FAIL_IC();
            }
            break;
          case GuardClassKind::Date:
            if (object->getClass() != &DateObject::class_) {
              FAIL_IC();
            }
            break;
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardAnyClass) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t claspOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSClass* clasp = reinterpret_cast<JSClass*>(
            stubInfo->getStubRawWord(cstub, claspOffset));
        if (obj->getClass() != clasp) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardGlobalGeneration) {
        uint32_t expectedOffset = cacheIRReader.stubOffset();
        uint32_t generationAddrOffset = cacheIRReader.stubOffset();
        uint32_t expected = stubInfo->getStubRawInt32(cstub, expectedOffset);
        uint32_t* generationAddr = reinterpret_cast<uint32_t*>(
            stubInfo->getStubRawWord(cstub, generationAddrOffset));
        if (*generationAddr != expected) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(HasClassResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t claspOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSClass* clasp = reinterpret_cast<JSClass*>(
            stubInfo->getStubRawWord(cstub, claspOffset));
        retValue = BooleanValue(obj->getClass() == clasp).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardCompartment) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t globalOffset = cacheIRReader.stubOffset();
        uint32_t compartmentOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSObject* global = reinterpret_cast<JSObject*>(
            stubInfo->getStubRawWord(cstub, globalOffset));
        JS::Compartment* compartment = reinterpret_cast<JS::Compartment*>(
            stubInfo->getStubRawWord(cstub, compartmentOffset));
        if (IsDeadProxyObject(global)) {
          FAIL_IC();
        }
        if (obj->compartment() != compartment) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsExtensible) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (obj->nonProxyIsExtensible()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNativeObject) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (!obj->is<NativeObject>()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsProxy) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (!obj->is<ProxyObject>()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNotProxy) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (obj->is<ProxyObject>()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNotArrayBufferMaybeShared) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        const JSClass* clasp = obj->getClass();
        if (clasp == &FixedLengthArrayBufferObject::class_ ||
            clasp == &FixedLengthSharedArrayBufferObject::class_ ||
            clasp == &ResizableArrayBufferObject::class_ ||
            clasp == &GrowableSharedArrayBufferObject::class_) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsTypedArray) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (!IsTypedArrayClass(obj->getClass())) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsFixedLengthTypedArray) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (!IsFixedLengthTypedArrayClass(obj->getClass())) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardHasProxyHandler) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t handlerOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        BaseProxyHandler* handler = reinterpret_cast<BaseProxyHandler*>(
            stubInfo->getStubRawWord(cstub, handlerOffset));
        if (obj->as<ProxyObject>().handler() != handler) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIsNotDOMProxy) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (obj->as<ProxyObject>().handler()->family() ==
            GetDOMProxyHandlerFamily()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardSpecificObject) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t expectedOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSObject* expected = reinterpret_cast<JSObject*>(
            stubInfo->getStubRawWord(cstub, expectedOffset));
        if (obj != expected) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardObjectIdentity) {
        ObjOperandId obj1Id = cacheIRReader.objOperandId();
        ObjOperandId obj2Id = cacheIRReader.objOperandId();
        JSObject* obj1 = reinterpret_cast<JSObject*>(READ_REG(obj1Id.id()));
        JSObject* obj2 = reinterpret_cast<JSObject*>(READ_REG(obj2Id.id()));
        if (obj1 != obj2) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardSpecificFunction) {
        ObjOperandId funId = cacheIRReader.objOperandId();
        uint32_t expectedOffset = cacheIRReader.stubOffset();
        uint32_t nargsAndFlagsOffset = cacheIRReader.stubOffset();
        (void)nargsAndFlagsOffset;  // Unused.
        uintptr_t expected = stubInfo->getStubRawWord(cstub, expectedOffset);
        if (expected != READ_REG(funId.id())) {
          FAIL_IC();
        }
        PREDICT_NEXT(LoadArgumentFixedSlot);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFunctionScript) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t expectedOffset = cacheIRReader.stubOffset();
        uint32_t nargsAndFlagsOffset = cacheIRReader.stubOffset();
        JSFunction* fun = reinterpret_cast<JSFunction*>(READ_REG(objId.id()));
        BaseScript* expected = reinterpret_cast<BaseScript*>(
            stubInfo->getStubRawWord(cstub, expectedOffset));
        (void)nargsAndFlagsOffset;

        if (!fun->hasBaseScript() || fun->baseScript() != expected) {
          FAIL_IC();
        }

        PREDICT_NEXT(CallScriptedFunction);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardSpecificAtom) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        uint32_t expectedOffset = cacheIRReader.stubOffset();
        uintptr_t expected = stubInfo->getStubRawWord(cstub, expectedOffset);
        if (expected != READ_REG(strId.id())) {
          // TODO: BaselineCacheIRCompiler also checks for equal strings
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardSpecificSymbol) {
        SymbolOperandId symId = cacheIRReader.symbolOperandId();
        uint32_t expectedOffset = cacheIRReader.stubOffset();
        uintptr_t expected = stubInfo->getStubRawWord(cstub, expectedOffset);
        if (expected != READ_REG(symId.id())) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardSpecificInt32) {
        Int32OperandId numId = cacheIRReader.int32OperandId();
        int32_t expected = cacheIRReader.int32Immediate();
        if (expected != int32_t(READ_REG(numId.id()))) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardNoDenseElements) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (obj->as<NativeObject>().getDenseInitializedLength() != 0) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardStringToIndex) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        int32_t result;
        if (str->hasIndexValue()) {
          uint32_t index = str->getIndexValue();
          MOZ_ASSERT(index <= INT32_MAX);
          result = index;
        } else {
          result = GetIndexFromString(str);
          if (result < 0) {
            FAIL_IC();
          }
        }
        WRITE_REG(resultId.id(), result, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardStringToInt32) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        int32_t result;
        // Use indexed value as fast path if possible.
        if (str->hasIndexValue()) {
          uint32_t index = str->getIndexValue();
          MOZ_ASSERT(index <= INT32_MAX);
          result = index;
        } else {
          if (!GetInt32FromStringPure(ctx.frameMgr.cxForLocalUseOnly(), str,
                                      &result)) {
            FAIL_IC();
          }
        }
        WRITE_REG(resultId.id(), result, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardStringToNumber) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        NumberOperandId resultId = cacheIRReader.numberOperandId();
        BOUNDSCHECK(resultId);
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        Value result;
        // Use indexed value as fast path if possible.
        if (str->hasIndexValue()) {
          uint32_t index = str->getIndexValue();
          MOZ_ASSERT(index <= INT32_MAX);
          result = Int32Value(index);
        } else {
          double value;
          if (!StringToNumberPure(ctx.frameMgr.cxForLocalUseOnly(), str,
                                  &value)) {
            FAIL_IC();
          }
          result = DoubleValue(value);
        }
        WRITE_VALUE_REG(resultId.id(), result);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(BooleanToNumber) {
        BooleanOperandId booleanId = cacheIRReader.booleanOperandId();
        NumberOperandId resultId = cacheIRReader.numberOperandId();
        BOUNDSCHECK(resultId);
        uint64_t boolean = READ_REG(booleanId.id());
        MOZ_ASSERT((boolean & ~1) == 0);
        WRITE_VALUE_REG(resultId.id(), Int32Value(boolean));
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardHasGetterSetter) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t idOffset = cacheIRReader.stubOffset();
        uint32_t getterSetterOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        jsid id = jsid::fromRawBits(stubInfo->getStubRawWord(cstub, idOffset));
        GetterSetter* getterSetter = reinterpret_cast<GetterSetter*>(
            stubInfo->getStubRawWord(cstub, getterSetterOffset));
        if (!ObjectHasGetterSetterPure(ctx.frameMgr.cxForLocalUseOnly(), obj,
                                       id, getterSetter)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardInt32IsNonNegative) {
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardDynamicSlotIsSpecificObject) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ObjOperandId expectedId = cacheIRReader.objOperandId();
        uint32_t slotOffset = cacheIRReader.stubOffset();
        JSObject* expected =
            reinterpret_cast<JSObject*>(READ_REG(expectedId.id()));
        uintptr_t slot = stubInfo->getStubRawInt32(cstub, slotOffset);
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        HeapSlot* slots = nobj->getSlotsUnchecked();
        // Note that unlike similar opcodes, GuardDynamicSlotIsSpecificObject
        // takes a slot index rather than a byte offset.
        Value actual = slots[slot];
        if (actual != ObjectValue(*expected)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardDynamicSlotIsNotObject) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t slotOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t slot = stubInfo->getStubRawInt32(cstub, slotOffset);
        NativeObject* nobj = &obj->as<NativeObject>();
        HeapSlot* slots = nobj->getSlotsUnchecked();
        // Note that unlike similar opcodes, GuardDynamicSlotIsNotObject takes a
        // slot index rather than a byte offset.
        Value actual = slots[slot];
        if (actual.isObject()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFixedSlotValue) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        uint32_t valOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        Value val =
            Value::fromRawBits(stubInfo->getStubRawInt64(cstub, valOffset));
        GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
            reinterpret_cast<uintptr_t>(obj) + offset);
        Value actual = slot->get();
        if (actual != val) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardDynamicSlotValue) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        uint32_t valOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        Value val =
            Value::fromRawBits(stubInfo->getStubRawInt64(cstub, valOffset));
        NativeObject* nobj = &obj->as<NativeObject>();
        HeapSlot* slots = nobj->getSlotsUnchecked();
        Value actual = slots[offset / sizeof(Value)];
        if (actual != val) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadFixedSlot) {
        ValOperandId resultId = cacheIRReader.valOperandId();
        BOUNDSCHECK(resultId);
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
            reinterpret_cast<uintptr_t>(obj) + offset);
        Value actual = slot->get();
        WRITE_VALUE_REG(resultId.id(), actual);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDynamicSlot) {
        ValOperandId resultId = cacheIRReader.valOperandId();
        BOUNDSCHECK(resultId);
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t slotOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t slot = stubInfo->getStubRawInt32(cstub, slotOffset);
        NativeObject* nobj = &obj->as<NativeObject>();
        HeapSlot* slots = nobj->getSlotsUnchecked();
        // Note that unlike similar opcodes, LoadDynamicSlot takes a slot index
        // rather than a byte offset.
        Value actual = slots[slot];
        WRITE_VALUE_REG(resultId.id(), actual);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardNoAllocationMetadataBuilder) {
        uint32_t builderAddrOffset = cacheIRReader.stubOffset();
        uintptr_t builderAddr =
            stubInfo->getStubRawWord(cstub, builderAddrOffset);
        if (*reinterpret_cast<uintptr_t*>(builderAddr) != 0) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFunctionHasJitEntry) {
        ObjOperandId funId = cacheIRReader.objOperandId();
        JSObject* fun = reinterpret_cast<JSObject*>(READ_REG(funId.id()));
        uint16_t flags = FunctionFlags::HasJitEntryFlags();
        if (!fun->as<JSFunction>().flags().hasFlags(flags)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFunctionHasNoJitEntry) {
        ObjOperandId funId = cacheIRReader.objOperandId();
        JSObject* fun = reinterpret_cast<JSObject*>(READ_REG(funId.id()));
        uint16_t flags = FunctionFlags::HasJitEntryFlags();
        if (fun->as<JSFunction>().flags().hasFlags(flags)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFunctionIsNonBuiltinCtor) {
        ObjOperandId funId = cacheIRReader.objOperandId();
        JSObject* fun = reinterpret_cast<JSObject*>(READ_REG(funId.id()));
        if (!fun->as<JSFunction>().isNonBuiltinConstructor()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardFunctionIsConstructor) {
        ObjOperandId funId = cacheIRReader.objOperandId();
        JSObject* fun = reinterpret_cast<JSObject*>(READ_REG(funId.id()));
        if (!fun->as<JSFunction>().isConstructor()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardNotClassConstructor) {
        ObjOperandId funId = cacheIRReader.objOperandId();
        JSObject* fun = reinterpret_cast<JSObject*>(READ_REG(funId.id()));
        if (fun->as<JSFunction>().isClassConstructor()) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardArrayIsPacked) {
        ObjOperandId arrayId = cacheIRReader.objOperandId();
        JSObject* array = reinterpret_cast<JSObject*>(READ_REG(arrayId.id()));
        if (!IsPackedArray(array)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardArgumentsObjectFlags) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint8_t flags = cacheIRReader.readByte();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (obj->as<ArgumentsObject>().hasFlags(flags)) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadObject) {
        ObjOperandId resultId = cacheIRReader.objOperandId();
        BOUNDSCHECK(resultId);
        uint32_t objOffset = cacheIRReader.stubOffset();
        intptr_t obj = stubInfo->getStubRawWord(cstub, objOffset);
        WRITE_REG(resultId.id(), obj, OBJECT);
        PREDICT_NEXT(GuardShape);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadProtoObject) {
        ObjOperandId resultId = cacheIRReader.objOperandId();
        BOUNDSCHECK(resultId);
        uint32_t protoObjOffset = cacheIRReader.stubOffset();
        ObjOperandId receiverObjId = cacheIRReader.objOperandId();
        (void)receiverObjId;
        intptr_t obj = stubInfo->getStubRawWord(cstub, protoObjOffset);
        WRITE_REG(resultId.id(), obj, OBJECT);
        PREDICT_NEXT(GuardShape);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadProto) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ObjOperandId resultId = cacheIRReader.objOperandId();
        BOUNDSCHECK(resultId);
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        WRITE_REG(resultId.id(),
                  reinterpret_cast<uint64_t>(nobj->staticPrototype()), OBJECT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadEnclosingEnvironment) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ObjOperandId resultId = cacheIRReader.objOperandId();
        BOUNDSCHECK(resultId);
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSObject* env = &obj->as<EnvironmentObject>().enclosingEnvironment();
        WRITE_REG(resultId.id(), reinterpret_cast<uint64_t>(env), OBJECT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadWrapperTarget) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ObjOperandId resultId = cacheIRReader.objOperandId();
        bool fallible = cacheIRReader.readBool();
        BOUNDSCHECK(resultId);
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSObject* target = obj->as<ProxyObject>().private_().toObjectOrNull();
        if (fallible && !target) {
          FAIL_IC();
        }
        WRITE_REG(resultId.id(), reinterpret_cast<uintptr_t>(target), OBJECT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadValueTag) {
        ValOperandId valId = cacheIRReader.valOperandId();
        ValueTagOperandId resultId = cacheIRReader.valueTagOperandId();
        BOUNDSCHECK(resultId);
        Value val = READ_VALUE_REG(valId.id());
        WRITE_REG(resultId.id(), val.asRawBits() >> JSVAL_TAG_SHIFT, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArgumentFixedSlot) {
        ValOperandId resultId = cacheIRReader.valOperandId();
        BOUNDSCHECK(resultId);
        uint8_t slotIndex = cacheIRReader.readByte();
        StackVal* sp = ctx.sp();
        Value val = sp[slotIndex].asValue();
        TRACE_PRINTF(" -> slot %d: val %" PRIx64 "\n", int(slotIndex),
                     val.asRawBits());
        WRITE_VALUE_REG(resultId.id(), val);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArgumentDynamicSlot) {
        ValOperandId resultId = cacheIRReader.valOperandId();
        BOUNDSCHECK(resultId);
        Int32OperandId argcId = cacheIRReader.int32OperandId();
        uint8_t slotIndex = cacheIRReader.readByte();
        int32_t argc = int32_t(READ_REG(argcId.id()));
        StackVal* sp = ctx.sp();
        Value val = sp[slotIndex + argc].asValue();
        WRITE_VALUE_REG(resultId.id(), val);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(TruncateDoubleToUInt32) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        Value input = READ_VALUE_REG(inputId.id());
        WRITE_REG(resultId.id(), JS::ToInt32(input.toNumber()), INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MegamorphicLoadSlotResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t nameOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        jsid name =
            jsid::fromRawBits(stubInfo->getStubRawWord(cstub, nameOffset));
        if (!obj->shape()->isNative()) {
          FAIL_IC();
        }
        Value result;
        if (!GetNativeDataPropertyPureWithCacheLookup(
                ctx.frameMgr.cxForLocalUseOnly(), obj, name, nullptr,
                &result)) {
          FAIL_IC();
        }
        retValue = result.asRawBits();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MegamorphicLoadSlotByValueResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ValOperandId idId = cacheIRReader.valOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        Value id = READ_VALUE_REG(idId.id());
        if (!obj->shape()->isNative()) {
          FAIL_IC();
        }
        Value values[2] = {id};
        if (!GetNativeDataPropertyByValuePure(ctx.frameMgr.cxForLocalUseOnly(),
                                              obj, nullptr, values)) {
          FAIL_IC();
        }
        retValue = values[1].asRawBits();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MegamorphicSetElement) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ValOperandId idId = cacheIRReader.valOperandId();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        bool strict = cacheIRReader.readBool();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        Value id = READ_VALUE_REG(idId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> obj0(&ctx.state.obj0, obj);
          ReservedRooted<Value> value0(&ctx.state.value0, id);
          ReservedRooted<Value> value1(&ctx.state.value1, rhs);
          if (!SetElementMegamorphic<false>(cx, obj0, value0, value1, strict)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StoreFixedSlot) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        uintptr_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
            reinterpret_cast<uintptr_t>(nobj) + offset);
        Value val = READ_VALUE_REG(rhsId.id());
        slot->set(val);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StoreDynamicSlot) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        uint32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        HeapSlot* slots = nobj->getSlotsUnchecked();
        Value val = READ_VALUE_REG(rhsId.id());
        size_t dynSlot = offset / sizeof(Value);
        size_t slot = dynSlot + nobj->numFixedSlots();
        slots[dynSlot].set(nobj, HeapSlot::Slot, slot, val);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(AddAndStoreFixedSlot) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        uint32_t newShapeOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        int32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        Value rhs = READ_VALUE_REG(rhsId.id());
        Shape* newShape = reinterpret_cast<Shape*>(
            stubInfo->getStubRawWord(cstub, newShapeOffset));
        obj->setShape(newShape);
        GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
            reinterpret_cast<uintptr_t>(obj) + offset);
        slot->init(rhs);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(AddAndStoreDynamicSlot) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        uint32_t newShapeOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        int32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        Value rhs = READ_VALUE_REG(rhsId.id());
        Shape* newShape = reinterpret_cast<Shape*>(
            stubInfo->getStubRawWord(cstub, newShapeOffset));
        NativeObject* nobj = &obj->as<NativeObject>();
        obj->setShape(newShape);
        HeapSlot* slots = nobj->getSlotsUnchecked();
        size_t dynSlot = offset / sizeof(Value);
        size_t slot = dynSlot + nobj->numFixedSlots();
        slots[dynSlot].init(nobj, HeapSlot::Slot, slot, rhs);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(AllocateAndStoreDynamicSlot) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        uint32_t newShapeOffset = cacheIRReader.stubOffset();
        uint32_t numNewSlotsOffset = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        int32_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        Value rhs = READ_VALUE_REG(rhsId.id());
        Shape* newShape = reinterpret_cast<Shape*>(
            stubInfo->getStubRawWord(cstub, newShapeOffset));
        int32_t numNewSlots =
            stubInfo->getStubRawInt32(cstub, numNewSlotsOffset);
        NativeObject* nobj = &obj->as<NativeObject>();
        // We have to (re)allocate dynamic slots. Do this first, as it's the
        // only fallible operation here. Note that growSlotsPure is fallible but
        // does not GC. Otherwise this is the same as AddAndStoreDynamicSlot
        // above.
        if (!NativeObject::growSlotsPure(ctx.frameMgr.cxForLocalUseOnly(), nobj,
                                         numNewSlots)) {
          FAIL_IC();
        }
        obj->setShape(newShape);
        HeapSlot* slots = nobj->getSlotsUnchecked();
        size_t dynSlot = offset / sizeof(Value);
        size_t slot = dynSlot + nobj->numFixedSlots();
        slots[dynSlot].init(nobj, HeapSlot::Slot, slot, rhs);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StoreDenseElement) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
          FAIL_IC();
        }
        HeapSlot* slot = &elems->elements()[index];
        if (slot->get().isMagic()) {
          FAIL_IC();
        }
        Value val = READ_VALUE_REG(rhsId.id());
        slot->set(nobj, HeapSlot::Element, index + elems->numShiftedElements(),
                  val);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StoreDenseElementHole) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        bool handleAdd = cacheIRReader.readBool();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t index = uint32_t(READ_REG(indexId.id()));
        Value rhs = READ_VALUE_REG(rhsId.id());
        NativeObject* nobj = &obj->as<NativeObject>();
        uint32_t initLength = nobj->getDenseInitializedLength();
        if (index < initLength) {
          nobj->setDenseElement(index, rhs);
        } else if (!handleAdd || index > initLength) {
          FAIL_IC();
        } else {
          if (index >= nobj->getDenseCapacity()) {
            if (!NativeObject::addDenseElementPure(
                    ctx.frameMgr.cxForLocalUseOnly(), nobj)) {
              FAIL_IC();
            }
          }
          nobj->setDenseInitializedLength(initLength + 1);

          // Baseline always updates the length field by directly accessing its
          // offset in ObjectElements. If the object is not an ArrayObject then
          // this field is never read, so it's okay to skip the update here in
          // that case.
          if (nobj->is<ArrayObject>()) {
            ArrayObject* aobj = &nobj->as<ArrayObject>();
            uint32_t len = aobj->length();
            if (len <= index) {
              aobj->setLength(ctx.frameMgr.cxForLocalUseOnly(), len + 1);
            }
          }

          nobj->initDenseElement(index, rhs);
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ArrayPush) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        Value rhs = READ_VALUE_REG(rhsId.id());
        ArrayObject* aobj = &obj->as<ArrayObject>();
        uint32_t initLength = aobj->getDenseInitializedLength();
        if (aobj->length() != initLength) {
          FAIL_IC();
        }
        if (initLength >= aobj->getDenseCapacity()) {
          if (!NativeObject::addDenseElementPure(
                  ctx.frameMgr.cxForLocalUseOnly(), aobj)) {
            FAIL_IC();
          }
        }
        aobj->setDenseInitializedLength(initLength + 1);
        aobj->setLengthToInitializedLength();
        aobj->initDenseElement(initLength, rhs);
        retValue = Int32Value(initLength + 1).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsObjectResult) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value val = READ_VALUE_REG(inputId.id());
        retValue = BooleanValue(val.isObject()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32MinMax) {
        bool isMax = cacheIRReader.readBool();
        Int32OperandId firstId = cacheIRReader.int32OperandId();
        Int32OperandId secondId = cacheIRReader.int32OperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        int32_t lhs = int32_t(READ_REG(firstId.id()));
        int32_t rhs = int32_t(READ_REG(secondId.id()));
        int32_t result = ((lhs > rhs) ^ isMax) ? rhs : lhs;
        WRITE_REG(resultId.id(), result, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StoreTypedArrayElement) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Scalar::Type elementType = cacheIRReader.scalarType();
        IntPtrOperandId indexId = cacheIRReader.intPtrOperandId();
        uint32_t rhsId = cacheIRReader.rawOperandId();
        bool handleOOB = cacheIRReader.readBool();
        ArrayBufferViewKind kind = cacheIRReader.arrayBufferViewKind();
        (void)kind;
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uintptr_t index = uintptr_t(READ_REG(indexId.id()));
        uint64_t rhs = READ_REG(rhsId);
        if (obj->as<TypedArrayObject>().length().isNothing()) {
          FAIL_IC();
        }
        if (index >= obj->as<TypedArrayObject>().length().value()) {
          if (!handleOOB) {
            FAIL_IC();
          }
        } else {
          Value v;
          switch (elementType) {
            case Scalar::Int8:
            case Scalar::Uint8:
            case Scalar::Int16:
            case Scalar::Uint16:
            case Scalar::Int32:
            case Scalar::Uint32:
            case Scalar::Uint8Clamped:
              v = Int32Value(rhs);
              break;

            case Scalar::Float16:
            case Scalar::Float32:
            case Scalar::Float64:
              v = Value::fromRawBits(rhs);
              MOZ_ASSERT(v.isNumber());
              break;

            case Scalar::BigInt64:
            case Scalar::BigUint64:
              v = BigIntValue(reinterpret_cast<JS::BigInt*>(rhs));
              break;

            case Scalar::MaxTypedArrayViewType:
            case Scalar::Int64:
            case Scalar::Simd128:
              MOZ_CRASH("Unsupported TypedArray type");
          }

          // SetTypedArrayElement doesn't do anything that can actually GC or
          // need a new context when the value can only be Int32, Double, or
          // BigInt, as the above switch statement enforces.
          FakeRooted<TypedArrayObject*> obj0(nullptr,
                                             &obj->as<TypedArrayObject>());
          FakeRooted<Value> value0(nullptr, v);
          ObjectOpResult result;
          MOZ_ASSERT(elementType == obj0->type());
          MOZ_ALWAYS_TRUE(SetTypedArrayElement(ctx.frameMgr.cxForLocalUseOnly(),
                                               obj0, index, value0, result));
          MOZ_ALWAYS_TRUE(result.ok());
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadTypedArrayElementExistsResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        IntPtrOperandId indexId = cacheIRReader.intPtrOperandId();
        ArrayBufferViewKind kind = cacheIRReader.arrayBufferViewKind();
        (void)kind;
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uintptr_t index = uintptr_t(READ_REG(indexId.id()));
        if (obj->as<TypedArrayObject>().length().isNothing()) {
          FAIL_IC();
        }
        retValue =
            BooleanValue(index < obj->as<TypedArrayObject>().length().value())
                .asRawBits();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadTypedArrayElementResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        IntPtrOperandId indexId = cacheIRReader.intPtrOperandId();
        Scalar::Type elementType = cacheIRReader.scalarType();
        bool handleOOB = cacheIRReader.readBool();
        bool forceDoubleForUint32 = cacheIRReader.readBool();
        ArrayBufferViewKind kind = cacheIRReader.arrayBufferViewKind();
        (void)kind;
        (void)elementType;
        (void)handleOOB;
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uintptr_t index = uintptr_t(READ_REG(indexId.id()));
        if (obj->as<TypedArrayObject>().length().isNothing()) {
          FAIL_IC();
        }
        if (index >= obj->as<TypedArrayObject>().length().value()) {
          FAIL_IC();
        }
        Value v;
        if (!obj->as<TypedArrayObject>().getElementPure(index, &v)) {
          FAIL_IC();
        }
        if (forceDoubleForUint32) {
          if (v.isInt32()) {
            v.setNumber(v.toInt32());
          }
        }
        retValue = v.asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallInt32ToString) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        StringOperandId resultId = cacheIRReader.stringOperandId();
        BOUNDSCHECK(resultId);
        int32_t input = int32_t(READ_REG(inputId.id()));
        JSLinearString* str =
            Int32ToStringPure(ctx.frameMgr.cxForLocalUseOnly(), input);
        if (str) {
          WRITE_REG(resultId.id(), reinterpret_cast<uintptr_t>(str), STRING);
        } else {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallScriptedFunction)
      CACHEOP_CASE_FALLTHROUGH(CallNativeFunction) {
        bool isNative = cacheop == CacheOp::CallNativeFunction;
        TRACE_PRINTF("CallScriptedFunction / CallNativeFunction (native: %d)\n",
                     isNative);
        ObjOperandId calleeId = cacheIRReader.objOperandId();
        Int32OperandId argcId = cacheIRReader.int32OperandId();
        CallFlags flags = cacheIRReader.callFlags();
        uint32_t argcFixed = cacheIRReader.uint32Immediate();
        bool ignoresRv = false;
        if (isNative) {
          ignoresRv = cacheIRReader.readBool();
        }

        TRACE_PRINTF("isConstructing = %d needsUninitializedThis = %d\n",
                     int(flags.isConstructing()),
                     int(flags.needsUninitializedThis()));

        JSFunction* callee =
            reinterpret_cast<JSFunction*>(READ_REG(calleeId.id()));
        uint32_t argc = uint32_t(READ_REG(argcId.id()));
        (void)argcFixed;

        if (!isNative) {
          if (!callee->hasBaseScript() ||
              !callee->baseScript()->hasBytecode() ||
              !callee->baseScript()->hasJitScript()) {
            FAIL_IC();
          }
        }

        // For now, fail any different-realm cases.
        if (!flags.isSameRealm()) {
          TRACE_PRINTF("failing: not same realm\n");
          FAIL_IC();
        }
        // And support only "standard" arg formats.
        if (flags.getArgFormat() != CallFlags::Standard) {
          TRACE_PRINTF("failing: not standard arg format\n");
          FAIL_IC();
        }

        // Fail constructing on a non-constructor callee.
        if (flags.isConstructing() && !callee->isConstructor()) {
          TRACE_PRINTF("failing: constructing a non-constructor\n");
          FAIL_IC();
        }

        // Handle arg-underflow (but only for scripted targets).
        uint32_t undefArgs = (!isNative && (argc < callee->nargs()))
                                 ? (callee->nargs() - argc)
                                 : 0;
        uint32_t extra = 1 + flags.isConstructing() + isNative;
        uint32_t totalArgs = argc + undefArgs + extra;
        StackVal* origArgs = ctx.sp();

        {
          PUSH_IC_FRAME();

          if (!ctx.stack.check(sp, sizeof(StackVal) * (totalArgs + 6))) {
            ReportOverRecursed(ctx.frameMgr.cxForLocalUseOnly());
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }

          // Create `this` if we are constructing and this is a
          // scripted function.
          Value thisVal;
          // Force JIT scripts to stick around, so we don't have to
          // fail the IC after GC'ing. This is critical, because
          // `stub` is not rooted (we don't have a BaselineStub frame
          // in PBL, only an exit frame directly below a baseline
          // function frame), so we cannot fall back to the next stub
          // once we pass this point.
          AutoKeepJitScripts keepJitScripts(cx);
          if (flags.isConstructing() && !isNative) {
            if (flags.needsUninitializedThis()) {
              thisVal = MagicValue(JS_UNINITIALIZED_LEXICAL);
            } else {
              ReservedRooted<JSObject*> calleeObj(&ctx.state.obj0, callee);
              ReservedRooted<JSObject*> newTargetRooted(
                  &ctx.state.obj1, &origArgs[0].asValue().toObject());
              ReservedRooted<Value> result(&ctx.state.value0);
              if (!CreateThisFromIC(cx, calleeObj, newTargetRooted, &result)) {
                ctx.error = PBIResult::Error;
                return IC_ERROR_SENTINEL();
              }
              thisVal = result;
              // `callee` may have moved.
              callee = &calleeObj->as<JSFunction>();
            }
          }
          // This will not be an Exit frame but a BaselineStub frame, so
          // replace the ExitFrameType with the ICStub pointer.
          POPNNATIVE(1);
          PUSHNATIVE(StackValNative(cstub));

          // `origArgs` is (in index order, i.e. increasing address order)
          // - normal, scripted: arg[argc-1] ... arg[0] thisv
          // - ctor, scripted: newTarget arg[argc-1] ... arg[0] thisv
          // - normal, native: arg[argc-1] ... arg[0] thisv callee
          // - ctor, native: newTarget arg[argc-1] ... arg[0] thisv callee
          //
          // and we need to push them in reverse order -- from sp
          // upward (in increasing address order) -- with args filled
          // in with `undefined` if fewer than the number of formals.

          // Push args: newTarget if constructing, extra undef's added
          // if underflow, then original args, and `callee` if
          // native. Replace `this` if constructing.
          if (flags.isConstructing()) {
            PUSH(origArgs[0]);
            origArgs++;
          }
          for (uint32_t i = 0; i < undefArgs; i++) {
            PUSH(StackVal(UndefinedValue()));
          }
          for (uint32_t i = 0; i < argc + 1 + isNative; i++) {
            PUSH(origArgs[i]);
          }
          if (flags.isConstructing() && !isNative) {
            sp[0] = StackVal(thisVal);
          }
          Value* args = reinterpret_cast<Value*>(sp);

          if (isNative) {
            PUSHNATIVE(StackValNative(argc));
            PUSHNATIVE(
                StackValNative(MakeFrameDescriptor(FrameType::BaselineStub)));

            // We *also* need an exit frame (the native baseline
            // execution would invoke a trampoline here).
            StackVal* trampolinePrevFP = ctx.stack.fp;
            PUSHNATIVE(StackValNative(nullptr));  // fake return address.
            PUSHNATIVE(StackValNative(ctx.stack.fp));
            ctx.stack.fp = sp;
            PUSHNATIVE(StackValNative(
                uint32_t(flags.isConstructing() ? ExitFrameType::ConstructNative
                                                : ExitFrameType::CallNative)));
            cx.getCx()->activation()->asJit()->setJSExitFP(
                reinterpret_cast<uint8_t*>(ctx.stack.fp));
            cx.getCx()->portableBaselineStack().top =
                reinterpret_cast<void*>(sp);

            JSNative native = ignoresRv
                                  ? callee->jitInfo()->ignoresReturnValueMethod
                                  : callee->native();
            bool success = native(cx, argc, args);

            ctx.stack.fp = trampolinePrevFP;
            POPNNATIVE(4);

            if (!success) {
              ctx.error = PBIResult::Error;
              return IC_ERROR_SENTINEL();
            }
            retValue = args[0].asRawBits();
          } else {
            TRACE_PRINTF("pushing callee: %p\n", callee);
            PUSHNATIVE(
                StackValNative(CalleeToToken(callee, flags.isConstructing())));

            PUSHNATIVE(StackValNative(
                MakeFrameDescriptorForJitCall(FrameType::BaselineStub, argc)));

            JSScript* script = callee->nonLazyScript();
            jsbytecode* pc = script->code();
            ImmutableScriptData* isd = script->immutableScriptData();
            PBIResult result;
            Value ret;
            result = PortableBaselineInterpret<false, kHybridICsInterp>(
                cx, ctx.state, ctx.stack, sp,
                /* envChain = */ nullptr, &ret, pc, isd, nullptr, nullptr,
                nullptr, PBIResult::Ok);
            if (result != PBIResult::Ok) {
              ctx.error = result;
              return IC_ERROR_SENTINEL();
            }
            if (flags.isConstructing() && !ret.isObject()) {
              ret = args[0];
            }
            retValue = ret.asRawBits();
          }
        }

        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallScriptedGetterResult)
      CACHEOP_CASE_FALLTHROUGH(CallScriptedSetter) {
        bool isSetter = cacheop == CacheOp::CallScriptedSetter;
        ObjOperandId receiverId = cacheIRReader.objOperandId();
        uint32_t getterSetterOffset = cacheIRReader.stubOffset();
        ValOperandId rhsId =
            isSetter ? cacheIRReader.valOperandId() : ValOperandId();
        bool sameRealm = cacheIRReader.readBool();
        uint32_t nargsAndFlagsOffset = cacheIRReader.stubOffset();
        (void)nargsAndFlagsOffset;

        Value receiver = isSetter ? ObjectValue(*reinterpret_cast<JSObject*>(
                                        READ_REG(receiverId.id())))
                                  : READ_VALUE_REG(receiverId.id());
        JSFunction* callee = reinterpret_cast<JSFunction*>(
            stubInfo->getStubRawWord(cstub, getterSetterOffset));
        Value rhs = isSetter ? READ_VALUE_REG(rhsId.id()) : UndefinedValue();

        if (!sameRealm) {
          FAIL_IC();
        }

        if (!callee->hasBaseScript() || !callee->baseScript()->hasBytecode() ||
            !callee->baseScript()->hasJitScript()) {
          FAIL_IC();
        }

        // For now, fail any arg-underflow case.
        if (callee->nargs() != isSetter ? 1 : 0) {
          TRACE_PRINTF(
              "failing: getter/setter does not have exactly 0/1 arg (has %d "
              "instead)\n",
              int(callee->nargs()));
          FAIL_IC();
        }

        {
          PUSH_IC_FRAME();

          if (!ctx.stack.check(sp, sizeof(StackVal) * 8)) {
            ReportOverRecursed(ctx.frameMgr.cxForLocalUseOnly());
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }

          // This will not be an Exit frame but a BaselineStub frame, so
          // replace the ExitFrameType with the ICStub pointer.
          POPNNATIVE(1);
          PUSHNATIVE(StackValNative(cstub));

          if (isSetter) {
            // Push arg: value.
            PUSH(StackVal(rhs));
          }
          TRACE_PRINTF("pushing receiver: %" PRIx64 "\n", receiver.asRawBits());
          // Push thisv: receiver.
          PUSH(StackVal(receiver));

          TRACE_PRINTF("pushing callee: %p\n", callee);
          PUSHNATIVE(StackValNative(
              CalleeToToken(callee, /* isConstructing = */ false)));

          PUSHNATIVE(StackValNative(MakeFrameDescriptorForJitCall(
              FrameType::BaselineStub, /* argc = */ isSetter ? 1 : 0)));

          JSScript* script = callee->nonLazyScript();
          jsbytecode* pc = script->code();
          ImmutableScriptData* isd = script->immutableScriptData();
          PBIResult result;
          Value ret;
          result = PortableBaselineInterpret<false, kHybridICsInterp>(
              cx, ctx.state, ctx.stack, sp, /* envChain = */ nullptr, &ret, pc,
              isd, nullptr, nullptr, nullptr, PBIResult::Ok);
          if (result != PBIResult::Ok) {
            ctx.error = result;
            return IC_ERROR_SENTINEL();
          }
          retValue = ret.asRawBits();
        }

        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallBoundScriptedFunction) {
        ObjOperandId calleeId = cacheIRReader.objOperandId();
        ObjOperandId targetId = cacheIRReader.objOperandId();
        Int32OperandId argcId = cacheIRReader.int32OperandId();
        CallFlags flags = cacheIRReader.callFlags();
        uint32_t numBoundArgs = cacheIRReader.uint32Immediate();

        BoundFunctionObject* boundFunc =
            reinterpret_cast<BoundFunctionObject*>(READ_REG(calleeId.id()));
        JSFunction* callee = &boundFunc->getTarget()->as<JSFunction>();
        uint32_t argc = uint32_t(READ_REG(argcId.id()));
        (void)targetId;

        if (!callee->hasBaseScript() || !callee->baseScript()->hasBytecode() ||
            !callee->baseScript()->hasJitScript()) {
          FAIL_IC();
        }

        // For now, fail any constructing or different-realm cases.
        if (flags.isConstructing()) {
          TRACE_PRINTF("failing: constructing\n");
          FAIL_IC();
        }
        if (!flags.isSameRealm()) {
          TRACE_PRINTF("failing: not same realm\n");
          FAIL_IC();
        }
        // And support only "standard" arg formats.
        if (flags.getArgFormat() != CallFlags::Standard) {
          TRACE_PRINTF("failing: not standard arg format\n");
          FAIL_IC();
        }

        uint32_t totalArgs = numBoundArgs + argc;

        // For now, fail any arg-underflow case.
        if (totalArgs < callee->nargs()) {
          TRACE_PRINTF("failing: too few args\n");
          FAIL_IC();
        }

        StackVal* origArgs = ctx.sp();

        {
          PUSH_IC_FRAME();

          if (!ctx.stack.check(sp, sizeof(StackVal) * (totalArgs + 6))) {
            ReportOverRecursed(ctx.frameMgr.cxForLocalUseOnly());
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }

          // This will not be an Exit frame but a BaselineStub frame, so
          // replace the ExitFrameType with the ICStub pointer.
          POPNNATIVE(1);
          PUSHNATIVE(StackValNative(cstub));

          // Push args.
          for (uint32_t i = 0; i < argc; i++) {
            PUSH(origArgs[i]);
          }
          // Push bound args.
          for (uint32_t i = 0; i < numBoundArgs; i++) {
            PUSH(StackVal(boundFunc->getBoundArg(numBoundArgs - 1 - i)));
          }
          // Push bound `this`.
          PUSH(StackVal(boundFunc->getBoundThis()));

          TRACE_PRINTF("pushing callee: %p\n", callee);
          PUSHNATIVE(StackValNative(
              CalleeToToken(callee, /* isConstructing = */ false)));

          PUSHNATIVE(StackValNative(MakeFrameDescriptorForJitCall(
              FrameType::BaselineStub, totalArgs)));

          JSScript* script = callee->nonLazyScript();
          jsbytecode* pc = script->code();
          ImmutableScriptData* isd = script->immutableScriptData();
          PBIResult result;
          Value ret;
          result = PortableBaselineInterpret<false, kHybridICsInterp>(
              cx, ctx.state, ctx.stack, sp, /* envChain = */ nullptr, &ret, pc,
              isd, nullptr, nullptr, nullptr, PBIResult::Ok);
          if (result != PBIResult::Ok) {
            ctx.error = result;
            return IC_ERROR_SENTINEL();
          }
          retValue = ret.asRawBits();
        }

        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MetaScriptedThisShape) {
        uint32_t thisShapeOffset = cacheIRReader.stubOffset();
        // This op is only metadata for the Warp Transpiler and should be
        // ignored.
        (void)thisShapeOffset;
        PREDICT_NEXT(CallScriptedFunction);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadFixedSlotResult)
      CACHEOP_CASE_FALLTHROUGH(LoadFixedSlotTypedResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        if (cacheop == CacheOp::LoadFixedSlotTypedResult) {
          // Type is unused here.
          (void)cacheIRReader.valueType();
        }
        uintptr_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        Value* slot = reinterpret_cast<Value*>(
            reinterpret_cast<uintptr_t>(nobj) + offset);
        TRACE_PRINTF(
            "LoadFixedSlotResult: obj %p offsetOffset %d offset %d slotPtr %p "
            "slot %" PRIx64 "\n",
            nobj, int(offsetOffset), int(offset), slot, slot->asRawBits());
        retValue = slot->asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDynamicSlotResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t offsetOffset = cacheIRReader.stubOffset();
        uintptr_t offset = stubInfo->getStubRawInt32(cstub, offsetOffset);
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        HeapSlot* slots = nobj->getSlotsUnchecked();
        retValue = slots[offset / sizeof(Value)].get().asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDenseElementResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
          FAIL_IC();
        }
        HeapSlot* slot = &elems->elements()[index];
        Value val = slot->get();
        if (val.isMagic()) {
          FAIL_IC();
        }
        retValue = val.asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDenseElementHoleResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
          FAIL_IC();
        }
        HeapSlot* slot = &elems->elements()[index];
        Value val = slot->get();
        if (val.isMagic()) {
          val.setUndefined();
        }
        retValue = val.asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDenseElementExistsResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
          FAIL_IC();
        }
        HeapSlot* slot = &elems->elements()[index];
        Value val = slot->get();
        if (val.isMagic()) {
          FAIL_IC();
        }
        retValue = BooleanValue(true).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDenseElementHoleExistsResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
          retValue = BooleanValue(false).asRawBits();
        } else {
          HeapSlot* slot = &elems->elements()[index];
          Value val = slot->get();
          retValue = BooleanValue(!val.isMagic()).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardIndexIsNotDenseElement) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t index = int32_t(READ_REG(indexId.id()));
        if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
          // OK -- not in the dense index range.
        } else {
          HeapSlot* slot = &elems->elements()[index];
          Value val = slot->get();
          if (!val.isMagic()) {
            // Not a magic value -- not the hole, so guard fails.
            FAIL_IC();
          }
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadInt32ArrayLengthResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayObject* aobj =
            reinterpret_cast<ArrayObject*>(READ_REG(objId.id()));
        uint32_t length = aobj->length();
        if (length > uint32_t(INT32_MAX)) {
          FAIL_IC();
        }
        retValue = Int32Value(length).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadInt32ArrayLength) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        ArrayObject* aobj =
            reinterpret_cast<ArrayObject*>(READ_REG(objId.id()));
        uint32_t length = aobj->length();
        if (length > uint32_t(INT32_MAX)) {
          FAIL_IC();
        }
        WRITE_REG(resultId.id(), length, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArrayBufferByteLengthInt32Result) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayBufferObject* abo =
            reinterpret_cast<ArrayBufferObject*>(READ_REG(objId.id()));
        size_t len = abo->byteLength();
        if (len > size_t(INT32_MAX)) {
          FAIL_IC();
        }
        retValue = Int32Value(int32_t(len)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArrayBufferByteLengthDoubleResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayBufferObject* abo =
            reinterpret_cast<ArrayBufferObject*>(READ_REG(objId.id()));
        size_t len = abo->byteLength();
        retValue = DoubleValue(double(len)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArrayBufferViewLengthInt32Result) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayBufferViewObject* abvo =
            reinterpret_cast<ArrayBufferViewObject*>(READ_REG(objId.id()));
        size_t len = size_t(
            abvo->getFixedSlot(ArrayBufferViewObject::LENGTH_SLOT).toPrivate());
        if (len > size_t(INT32_MAX)) {
          FAIL_IC();
        }
        retValue = Int32Value(int32_t(len)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArrayBufferViewLengthDoubleResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayBufferViewObject* abvo =
            reinterpret_cast<ArrayBufferViewObject*>(READ_REG(objId.id()));
        size_t len = size_t(
            abvo->getFixedSlot(ArrayBufferViewObject::LENGTH_SLOT).toPrivate());
        retValue = DoubleValue(double(len)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArgumentsObjectArgResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        uint32_t index = uint32_t(READ_REG(indexId.id()));
        ArgumentsObject* args = &obj->as<ArgumentsObject>();
        if (index >= args->initialLength() || args->hasOverriddenElement()) {
          FAIL_IC();
        }
        if (args->argIsForwarded(index)) {
          FAIL_IC();
        }
        retValue = args->arg(index).asRawBits();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LinearizeForCharAccess) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        StringOperandId resultId = cacheIRReader.stringOperandId();
        BOUNDSCHECK(resultId);
        JSString* str = reinterpret_cast<JSLinearString*>(READ_REG(strId.id()));
        (void)indexId;

        WRITE_REG(resultId.id(), reinterpret_cast<uintptr_t>(str), STRING);
        if (str->isRope()) {
          PUSH_IC_FRAME();
          JSLinearString* result = LinearizeForCharAccess(cx, str);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          WRITE_REG(resultId.id(), reinterpret_cast<uintptr_t>(result), STRING);
        }
        PREDICT_NEXT(LoadStringCharResult);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LinearizeForCodePointAccess) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        StringOperandId resultId = cacheIRReader.stringOperandId();
        BOUNDSCHECK(resultId);
        JSString* str = reinterpret_cast<JSLinearString*>(READ_REG(strId.id()));
        (void)indexId;

        WRITE_REG(resultId.id(), reinterpret_cast<uintptr_t>(str), STRING);
        if (str->isRope()) {
          PUSH_IC_FRAME();
          JSLinearString* result = LinearizeForCharAccess(cx, str);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          WRITE_REG(resultId.id(), reinterpret_cast<uintptr_t>(result), STRING);
        }
        PREDICT_NEXT(LoadStringCodePointResult);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadStringCharResult)
      CACHEOP_CASE_FALLTHROUGH(LoadStringAtResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        bool handleOOB = cacheIRReader.readBool();

        JSString* str = reinterpret_cast<JSLinearString*>(READ_REG(strId.id()));
        int32_t index = int32_t(READ_REG(indexId.id()));
        JSString* result = nullptr;
        if (index < 0 || size_t(index) >= str->length()) {
          if (handleOOB) {
            if (cacheop == CacheOp::LoadStringCharResult) {
              // Return an empty string.
              retValue =
                  StringValue(ctx.frameMgr.cxForLocalUseOnly()->names().empty_)
                      .asRawBits();
            } else {
              // Return `undefined`.
              retValue = UndefinedValue().asRawBits();
            }
          } else {
            FAIL_IC();
          }
        } else {
          char16_t c;
          // Guaranteed to always work because this CacheIR op is
          // always preceded by LinearizeForCharAccess.
          MOZ_ALWAYS_TRUE(str->getChar(/* cx = */ nullptr, index, &c));
          StaticStrings& sstr =
              ctx.frameMgr.cxForLocalUseOnly()->staticStrings();
          if (sstr.hasUnit(c)) {
            result = sstr.getUnit(c);
          } else {
            PUSH_IC_FRAME();
            result = StringFromCharCode(cx, c);
            if (!result) {
              ctx.error = PBIResult::Error;
              return IC_ERROR_SENTINEL();
            }
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadStringCharCodeResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        bool handleOOB = cacheIRReader.readBool();

        JSString* str = reinterpret_cast<JSLinearString*>(READ_REG(strId.id()));
        int32_t index = int32_t(READ_REG(indexId.id()));
        Value result;
        if (index < 0 || size_t(index) >= str->length()) {
          if (handleOOB) {
            // Return NaN.
            result = JS::NaNValue();
          } else {
            FAIL_IC();
          }
        } else {
          char16_t c;
          // Guaranteed to always work because this CacheIR op is
          // always preceded by LinearizeForCharAccess.
          MOZ_ALWAYS_TRUE(str->getChar(/* cx = */ nullptr, index, &c));
          result = Int32Value(c);
        }
        retValue = result.asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadStringCodePointResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        bool handleOOB = cacheIRReader.readBool();

        JSString* str = reinterpret_cast<JSLinearString*>(READ_REG(strId.id()));
        int32_t index = int32_t(READ_REG(indexId.id()));
        Value result;
        if (index < 0 || size_t(index) >= str->length()) {
          if (handleOOB) {
            // Return undefined.
            result = UndefinedValue();
          } else {
            FAIL_IC();
          }
        } else {
          char32_t c;
          // Guaranteed to be always work because this CacheIR op is
          // always preceded by LinearizeForCharAccess.
          MOZ_ALWAYS_TRUE(str->getCodePoint(/* cx = */ nullptr, index, &c));
          result = Int32Value(c);
        }
        retValue = result.asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadStringLengthResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        size_t length = str->length();
        if (length > size_t(INT32_MAX)) {
          FAIL_IC();
        }
        retValue = Int32Value(length).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadObjectResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        retValue =
            ObjectValue(*reinterpret_cast<JSObject*>(READ_REG(objId.id())))
                .asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadStringResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        retValue =
            StringValue(reinterpret_cast<JSString*>(READ_REG(strId.id())))
                .asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadSymbolResult) {
        SymbolOperandId symId = cacheIRReader.symbolOperandId();
        retValue =
            SymbolValue(reinterpret_cast<JS::Symbol*>(READ_REG(symId.id())))
                .asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadInt32Result) {
        Int32OperandId valId = cacheIRReader.int32OperandId();
        retValue = Int32Value(READ_REG(valId.id())).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDoubleResult) {
        NumberOperandId valId = cacheIRReader.numberOperandId();
        Value val = READ_VALUE_REG(valId.id());
        if (val.isInt32()) {
          val = DoubleValue(val.toInt32());
        }
        retValue = val.asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadBigIntResult) {
        BigIntOperandId valId = cacheIRReader.bigIntOperandId();
        retValue =
            BigIntValue(reinterpret_cast<JS::BigInt*>(READ_REG(valId.id())))
                .asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadBooleanResult) {
        bool val = cacheIRReader.readBool();
        retValue = BooleanValue(val).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadInt32Constant) {
        uint32_t valOffset = cacheIRReader.stubOffset();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        uint32_t value = stubInfo->getStubRawInt32(cstub, valOffset);
        WRITE_REG(resultId.id(), value, INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadConstantStringResult) {
        uint32_t strOffset = cacheIRReader.stubOffset();
        JSString* str = reinterpret_cast<JSString*>(
            stubInfo->getStubRawWord(cstub, strOffset));
        retValue = StringValue(str).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoubleAddResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        retValue = DoubleValue(lhs.toNumber() + rhs.toNumber()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoubleSubResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        retValue = DoubleValue(lhs.toNumber() - rhs.toNumber()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoubleMulResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        retValue = DoubleValue(lhs.toNumber() * rhs.toNumber()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoubleDivResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        retValue =
            DoubleValue(NumberDiv(lhs.toNumber(), rhs.toNumber())).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoubleModResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        retValue =
            DoubleValue(NumberMod(lhs.toNumber(), rhs.toNumber())).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoublePowResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        Value rhs = READ_VALUE_REG(rhsId.id());
        retValue =
            DoubleValue(ecmaPow(lhs.toNumber(), rhs.toNumber())).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

#define INT32_OP(name, op, extra_check)                    \
  CACHEOP_CASE(Int32##name##Result) {                      \
    Int32OperandId lhsId = cacheIRReader.int32OperandId(); \
    Int32OperandId rhsId = cacheIRReader.int32OperandId(); \
    int64_t lhs = int64_t(int32_t(READ_REG(lhsId.id())));  \
    int64_t rhs = int64_t(int32_t(READ_REG(rhsId.id())));  \
    extra_check;                                           \
    int64_t result = lhs op rhs;                           \
    if (result < INT32_MIN || result > INT32_MAX) {        \
      FAIL_IC();                                           \
    }                                                      \
    retValue = Int32Value(int32_t(result)).asRawBits();    \
    PREDICT_RETURN();                                      \
    DISPATCH_CACHEOP();                                    \
  }

      // clang-format off
  INT32_OP(Add, +, {});
  INT32_OP(Sub, -, {});
      // clang-format on
      INT32_OP(Mul, *, {
        if (rhs * lhs == 0 && ((rhs < 0) ^ (lhs < 0))) {
          FAIL_IC();
        }
      });
      INT32_OP(Div, /, {
        if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) {
          FAIL_IC();
        }
        if (lhs == 0 && rhs < 0) {
          FAIL_IC();
        }
        if (lhs % rhs != 0) {
          FAIL_IC();
        }
      });
      INT32_OP(Mod, %, {
        if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) {
          FAIL_IC();
        }
        if (lhs % rhs == 0 && lhs < 0) {
          FAIL_IC();
        }
      });
      // clang-format off
  INT32_OP(BitOr, |, {});
  INT32_OP(BitXor, ^, {});
  INT32_OP(BitAnd, &, {});
      // clang-format on

      CACHEOP_CASE(Int32PowResult) {
        Int32OperandId lhsId = cacheIRReader.int32OperandId();
        Int32OperandId rhsId = cacheIRReader.int32OperandId();
        int64_t lhs = int64_t(int32_t(READ_REG(lhsId.id())));
        uint64_t rhs = uint64_t(int32_t(READ_REG(rhsId.id())));
        int64_t result;

        if (lhs == 1) {
          result = 1;
        } else if (rhs >= uint64_t(INT64_MIN)) {
          FAIL_IC();
        } else {
          result = 1;
          int64_t runningSquare = lhs;
          while (rhs) {
            if (rhs & 1) {
              result *= runningSquare;
              if (result > int64_t(INT32_MAX)) {
                FAIL_IC();
              }
            }
            rhs >>= 1;
            if (rhs == 0) {
              break;
            }
            runningSquare *= runningSquare;
            if (runningSquare > int64_t(INT32_MAX)) {
              FAIL_IC();
            }
          }
        }

        retValue = Int32Value(int32_t(result)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32IncResult) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        int64_t value = int64_t(int32_t(READ_REG(inputId.id())));
        value++;
        if (value > INT32_MAX) {
          FAIL_IC();
        }
        retValue = Int32Value(int32_t(value)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32LeftShiftResult) {
        Int32OperandId lhsId = cacheIRReader.int32OperandId();
        Int32OperandId rhsId = cacheIRReader.int32OperandId();
        int32_t lhs = int32_t(READ_REG(lhsId.id()));
        int32_t rhs = int32_t(READ_REG(rhsId.id()));
        int32_t result = lhs << (rhs & 0x1F);
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32RightShiftResult) {
        Int32OperandId lhsId = cacheIRReader.int32OperandId();
        Int32OperandId rhsId = cacheIRReader.int32OperandId();
        int32_t lhs = int32_t(READ_REG(lhsId.id()));
        int32_t rhs = int32_t(READ_REG(rhsId.id()));
        int32_t result = lhs >> (rhs & 0x1F);
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32URightShiftResult) {
        Int32OperandId lhsId = cacheIRReader.int32OperandId();
        Int32OperandId rhsId = cacheIRReader.int32OperandId();
        bool forceDouble = cacheIRReader.readBool();
        (void)forceDouble;
        uint32_t lhs = uint32_t(READ_REG(lhsId.id()));
        int32_t rhs = int32_t(READ_REG(rhsId.id()));
        uint32_t result = lhs >> (rhs & 0x1F);
        retValue = (result >= 0x80000000)
                       ? DoubleValue(double(result)).asRawBits()
                       : Int32Value(int32_t(result)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32NotResult) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        int32_t input = int32_t(READ_REG(inputId.id()));
        retValue = Int32Value(~input).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadInt32TruthyResult) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        int32_t val = int32_t(READ_REG(inputId.id()));
        retValue = BooleanValue(val != 0).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDoubleTruthyResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        // NaN is falsy, not truthy.
        retValue = BooleanValue(input != 0.0 && !std::isnan(input)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadStringTruthyResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSLinearString*>(READ_REG(strId.id()));
        retValue = BooleanValue(str->length() > 0).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadObjectTruthyResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        const JSClass* cls = obj->getClass();
        if (cls->isProxyObject()) {
          FAIL_IC();
        }
        retValue = BooleanValue(!cls->emulatesUndefined()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadValueResult) {
        uint32_t valOffset = cacheIRReader.stubOffset();
        retValue = stubInfo->getStubRawInt64(cstub, valOffset);
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadOperandResult) {
        ValOperandId inputId = cacheIRReader.valOperandId();
        retValue = READ_REG(inputId.id());
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ConcatStringsResult) {
        StringOperandId lhsId = cacheIRReader.stringOperandId();
        StringOperandId rhsId = cacheIRReader.stringOperandId();
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> lhs(
              &ctx.state.str0,
              reinterpret_cast<JSString*>(READ_REG(lhsId.id())));
          ReservedRooted<JSString*> rhs(
              &ctx.state.str1,
              reinterpret_cast<JSString*>(READ_REG(rhsId.id())));
          JSString* result =
              ConcatStrings<CanGC>(ctx.frameMgr.cxForLocalUseOnly(), lhs, rhs);
          if (result) {
            retValue = StringValue(result).asRawBits();
          } else {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CompareStringResult) {
        JSOp op = cacheIRReader.jsop();
        StringOperandId lhsId = cacheIRReader.stringOperandId();
        StringOperandId rhsId = cacheIRReader.stringOperandId();
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> lhs(
              &ctx.state.str0,
              reinterpret_cast<JSString*>(READ_REG(lhsId.id())));
          ReservedRooted<JSString*> rhs(
              &ctx.state.str1,
              reinterpret_cast<JSString*>(READ_REG(rhsId.id())));
          bool result;
          if (lhs == rhs) {
            // If operands point to the same instance, the strings are trivially
            // equal.
            result = op == JSOp::Eq || op == JSOp::StrictEq || op == JSOp::Le ||
                     op == JSOp::Ge;
          } else {
            switch (op) {
              case JSOp::Eq:
              case JSOp::StrictEq:
                if (lhs->isAtom() && rhs->isAtom()) {
                  result = false;
                  break;
                }
                if (lhs->length() != rhs->length()) {
                  result = false;
                  break;
                }
                if (!StringsEqual<EqualityKind::Equal>(cx, lhs, rhs, &result)) {
                  ctx.error = PBIResult::Error;
                  return IC_ERROR_SENTINEL();
                }
                break;
              case JSOp::Ne:
              case JSOp::StrictNe:
                if (lhs->isAtom() && rhs->isAtom()) {
                  result = true;
                  break;
                }
                if (lhs->length() != rhs->length()) {
                  result = true;
                  break;
                }
                if (!StringsEqual<EqualityKind::NotEqual>(cx, lhs, rhs,
                                                          &result)) {
                  ctx.error = PBIResult::Error;
                  return IC_ERROR_SENTINEL();
                }
                break;
              case JSOp::Lt:
                if (!StringsCompare<ComparisonKind::LessThan>(cx, lhs, rhs,
                                                              &result)) {
                  ctx.error = PBIResult::Error;
                  return IC_ERROR_SENTINEL();
                }
                break;
              case JSOp::Ge:
                if (!StringsCompare<ComparisonKind::GreaterThanOrEqual>(
                        cx, lhs, rhs, &result)) {
                  ctx.error = PBIResult::Error;
                  return IC_ERROR_SENTINEL();
                }
                break;
              case JSOp::Le:
                if (!StringsCompare<ComparisonKind::GreaterThanOrEqual>(
                        cx, /* N.B. swapped order */ rhs, lhs, &result)) {
                  ctx.error = PBIResult::Error;
                  return IC_ERROR_SENTINEL();
                }
                break;
              case JSOp::Gt:
                if (!StringsCompare<ComparisonKind::LessThan>(
                        cx, /* N.B. swapped order */ rhs, lhs, &result)) {
                  ctx.error = PBIResult::Error;
                  return IC_ERROR_SENTINEL();
                }
                break;
              default:
                MOZ_CRASH("bad opcode");
            }
          }
          retValue = BooleanValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CompareInt32Result) {
        JSOp op = cacheIRReader.jsop();
        Int32OperandId lhsId = cacheIRReader.int32OperandId();
        Int32OperandId rhsId = cacheIRReader.int32OperandId();
        int64_t lhs = int64_t(int32_t(READ_REG(lhsId.id())));
        int64_t rhs = int64_t(int32_t(READ_REG(rhsId.id())));
        TRACE_PRINTF("lhs (%d) = %" PRIi64 " rhs (%d) = %" PRIi64 "\n",
                     lhsId.id(), lhs, rhsId.id(), rhs);
        bool result;
        switch (op) {
          case JSOp::Eq:
          case JSOp::StrictEq:
            result = lhs == rhs;
            break;
          case JSOp::Ne:
          case JSOp::StrictNe:
            result = lhs != rhs;
            break;
          case JSOp::Lt:
            result = lhs < rhs;
            break;
          case JSOp::Le:
            result = lhs <= rhs;
            break;
          case JSOp::Gt:
            result = lhs > rhs;
            break;
          case JSOp::Ge:
            result = lhs >= rhs;
            break;
          default:
            MOZ_CRASH("Unexpected opcode");
        }
        retValue = BooleanValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CompareDoubleResult) {
        JSOp op = cacheIRReader.jsop();
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        double lhs = READ_VALUE_REG(lhsId.id()).toNumber();
        double rhs = READ_VALUE_REG(rhsId.id()).toNumber();
        bool result;
        switch (op) {
          case JSOp::Eq:
          case JSOp::StrictEq:
            result = lhs == rhs;
            break;
          case JSOp::Ne:
          case JSOp::StrictNe:
            result = lhs != rhs;
            break;
          case JSOp::Lt:
            result = lhs < rhs;
            break;
          case JSOp::Le:
            result = lhs <= rhs;
            break;
          case JSOp::Gt:
            result = lhs > rhs;
            break;
          case JSOp::Ge:
            result = lhs >= rhs;
            break;
          default:
            MOZ_CRASH("Unexpected opcode");
        }
        retValue = BooleanValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CompareNullUndefinedResult) {
        JSOp op = cacheIRReader.jsop();
        bool isUndefined = cacheIRReader.readBool();
        ValOperandId inputId = cacheIRReader.valOperandId();
        Value val = READ_VALUE_REG(inputId.id());
        if (val.isObject() && val.toObject().getClass()->isProxyObject()) {
          FAIL_IC();
        }

        bool result;
        switch (op) {
          case JSOp::Eq:
            result = val.isUndefined() || val.isNull() ||
                     (val.isObject() &&
                      val.toObject().getClass()->emulatesUndefined());
            break;
          case JSOp::Ne:
            result = !(val.isUndefined() || val.isNull() ||
                       (val.isObject() &&
                        val.toObject().getClass()->emulatesUndefined()));
            break;
          case JSOp::StrictEq:
            result = isUndefined ? val.isUndefined() : val.isNull();
            break;
          case JSOp::StrictNe:
            result = !(isUndefined ? val.isUndefined() : val.isNull());
            break;
          default:
            MOZ_CRASH("bad opcode");
        }
        retValue = BooleanValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CompareObjectResult) {
        JSOp op = cacheIRReader.jsop();
        ObjOperandId lhsId = cacheIRReader.objOperandId();
        ObjOperandId rhsId = cacheIRReader.objOperandId();
        (void)op;
        JSObject* lhs = reinterpret_cast<JSObject*>(READ_REG(lhsId.id()));
        JSObject* rhs = reinterpret_cast<JSObject*>(READ_REG(rhsId.id()));
        switch (op) {
          case JSOp::Eq:
          case JSOp::StrictEq:
            retValue = BooleanValue(lhs == rhs).asRawBits();
            break;
          case JSOp::Ne:
          case JSOp::StrictNe:
            retValue = BooleanValue(lhs != rhs).asRawBits();
            break;
          default:
            FAIL_IC();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CompareSymbolResult) {
        JSOp op = cacheIRReader.jsop();
        SymbolOperandId lhsId = cacheIRReader.symbolOperandId();
        SymbolOperandId rhsId = cacheIRReader.symbolOperandId();
        (void)op;
        JS::Symbol* lhs = reinterpret_cast<JS::Symbol*>(READ_REG(lhsId.id()));
        JS::Symbol* rhs = reinterpret_cast<JS::Symbol*>(READ_REG(rhsId.id()));
        switch (op) {
          case JSOp::Eq:
          case JSOp::StrictEq:
            retValue = BooleanValue(lhs == rhs).asRawBits();
            break;
          case JSOp::Ne:
          case JSOp::StrictNe:
            retValue = BooleanValue(lhs != rhs).asRawBits();
            break;
          default:
            FAIL_IC();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(AssertPropertyLookup) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t idOffset = cacheIRReader.stubOffset();
        uint32_t slotOffset = cacheIRReader.stubOffset();
        // Debug-only assertion; we can ignore.
        (void)objId;
        (void)idOffset;
        (void)slotOffset;
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathSqrtNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        retValue = NumberValue(sqrt(input)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathAbsInt32Result) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        int32_t input = int32_t(READ_REG(inputId.id()));
        if (input == INT32_MIN) {
          FAIL_IC();
        }
        if (input < 0) {
          input = -input;
        }
        retValue = Int32Value(input).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathAbsNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        retValue = DoubleValue(fabs(input)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathClz32Result) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        int32_t input = int32_t(READ_REG(inputId.id()));
        int32_t result =
            (input == 0) ? 32 : mozilla::CountLeadingZeroes32(input);
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathSignInt32Result) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        int32_t input = int32_t(READ_REG(inputId.id()));
        int32_t result = (input == 0) ? 0 : ((input > 0) ? 1 : -1);
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathSignNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        double result = 0;
        if (std::isnan(input)) {
          result = JS::GenericNaN();
        } else if (input == 0 && std::signbit(input)) {
          result = -0.0;
        } else if (input == 0) {
          result = 0;
        } else if (input > 0) {
          result = 1;
        } else {
          result = -1;
        }
        retValue = DoubleValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathSignNumberToInt32Result) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        int32_t result = 0;
        if (std::isnan(input) || (input == 0.0 && std::signbit(input))) {
          FAIL_IC();
        } else if (input == 0) {
          result = 0;
        } else if (input > 0) {
          result = 1;
        } else {
          result = -1;
        }
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathImulResult) {
        Int32OperandId lhsId = cacheIRReader.int32OperandId();
        Int32OperandId rhsId = cacheIRReader.int32OperandId();
        int32_t lhs = int32_t(READ_REG(lhsId.id()));
        int32_t rhs = int32_t(READ_REG(rhsId.id()));
        int32_t result = lhs * rhs;
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathFRoundNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        retValue = DoubleValue(double(float(input))).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathRandomResult) {
        uint32_t rngOffset = cacheIRReader.stubOffset();
        auto* rng = reinterpret_cast<mozilla::non_crypto::XorShift128PlusRNG*>(
            stubInfo->getStubRawWord(cstub, rngOffset));
        retValue = DoubleValue(rng->nextDouble()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathHypot2NumberResult) {
        NumberOperandId firstId = cacheIRReader.numberOperandId();
        double first = READ_VALUE_REG(firstId.id()).toNumber();
        NumberOperandId secondId = cacheIRReader.numberOperandId();
        double second = READ_VALUE_REG(secondId.id()).toNumber();
        retValue = DoubleValue(ecmaHypot(first, second)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathHypot3NumberResult) {
        NumberOperandId firstId = cacheIRReader.numberOperandId();
        double first = READ_VALUE_REG(firstId.id()).toNumber();
        NumberOperandId secondId = cacheIRReader.numberOperandId();
        double second = READ_VALUE_REG(secondId.id()).toNumber();
        NumberOperandId thirdId = cacheIRReader.numberOperandId();
        double third = READ_VALUE_REG(thirdId.id()).toNumber();
        retValue = DoubleValue(hypot3(first, second, third)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathHypot4NumberResult) {
        NumberOperandId firstId = cacheIRReader.numberOperandId();
        double first = READ_VALUE_REG(firstId.id()).toNumber();
        NumberOperandId secondId = cacheIRReader.numberOperandId();
        double second = READ_VALUE_REG(secondId.id()).toNumber();
        NumberOperandId thirdId = cacheIRReader.numberOperandId();
        double third = READ_VALUE_REG(thirdId.id()).toNumber();
        NumberOperandId fourthId = cacheIRReader.numberOperandId();
        double fourth = READ_VALUE_REG(fourthId.id()).toNumber();
        retValue =
            DoubleValue(hypot4(first, second, third, fourth)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathAtan2NumberResult) {
        NumberOperandId lhsId = cacheIRReader.numberOperandId();
        double lhs = READ_VALUE_REG(lhsId.id()).toNumber();
        NumberOperandId rhsId = cacheIRReader.numberOperandId();
        double rhs = READ_VALUE_REG(rhsId.id()).toNumber();
        retValue = DoubleValue(ecmaAtan2(lhs, rhs)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathFloorNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        double result = fdlibm_floor(input);
        retValue = DoubleValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathCeilNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        double result = fdlibm_ceil(input);
        retValue = DoubleValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathTruncNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        double result = fdlibm_trunc(input);
        retValue = DoubleValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathFloorToInt32Result) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        if (input == 0.0 && std::signbit(input)) {
          FAIL_IC();
        }
        double result = fdlibm_floor(input);
        int32_t intResult = int32_t(result);
        if (double(intResult) != result) {
          FAIL_IC();
        }
        retValue = Int32Value(intResult).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathCeilToInt32Result) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        if (input > -1.0 && std::signbit(input)) {
          FAIL_IC();
        }
        double result = fdlibm_ceil(input);
        int32_t intResult = int32_t(result);
        if (double(intResult) != result) {
          FAIL_IC();
        }
        retValue = Int32Value(intResult).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathTruncToInt32Result) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        if (input == 0.0 && std::signbit(input)) {
          FAIL_IC();
        }
        double result = fdlibm_trunc(input);
        int32_t intResult = int32_t(result);
        if (double(intResult) != result) {
          FAIL_IC();
        }
        retValue = Int32Value(intResult).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathRoundToInt32Result) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        if (input == 0.0 && std::signbit(input)) {
          FAIL_IC();
        }
        int32_t intResult = int32_t(input);
        if (double(intResult) != input) {
          FAIL_IC();
        }
        retValue = Int32Value(intResult).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NumberMinMax) {
        bool isMax = cacheIRReader.readBool();
        NumberOperandId firstId = cacheIRReader.numberOperandId();
        NumberOperandId secondId = cacheIRReader.numberOperandId();
        NumberOperandId resultId = cacheIRReader.numberOperandId();
        BOUNDSCHECK(resultId);
        double first = READ_VALUE_REG(firstId.id()).toNumber();
        double second = READ_VALUE_REG(secondId.id()).toNumber();
        double result = DoubleMinMax(isMax, first, second);
        WRITE_VALUE_REG(resultId.id(), DoubleValue(result));
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32MinMaxArrayResult) {
        ObjOperandId arrayId = cacheIRReader.objOperandId();
        bool isMax = cacheIRReader.readBool();
        // ICs that use this opcode depend on implicit unboxing due to
        // type-overload on ObjOperandId when a value is loaded
        // directly from an argument slot. We explicitly unbox here.
        NativeObject* nobj = reinterpret_cast<NativeObject*>(
            &READ_VALUE_REG(arrayId.id()).toObject());
        uint32_t len = nobj->getDenseInitializedLength();
        if (len == 0) {
          FAIL_IC();
        }
        ObjectElements* elems = nobj->getElementsHeader();
        int32_t accum = 0;
        for (uint32_t i = 0; i < len; i++) {
          HeapSlot* slot = &elems->elements()[i];
          Value val = slot->get();
          if (!val.isInt32()) {
            FAIL_IC();
          }
          int32_t valInt = val.toInt32();
          if (i > 0) {
            accum = isMax ? ((valInt > accum) ? valInt : accum)
                          : ((valInt < accum) ? valInt : accum);
          } else {
            accum = valInt;
          }
        }
        retValue = Int32Value(accum).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NumberMinMaxArrayResult) {
        ObjOperandId arrayId = cacheIRReader.objOperandId();
        bool isMax = cacheIRReader.readBool();
        // ICs that use this opcode depend on implicit unboxing due to
        // type-overload on ObjOperandId when a value is loaded
        // directly from an argument slot. We explicitly unbox here.
        NativeObject* nobj = reinterpret_cast<NativeObject*>(
            &READ_VALUE_REG(arrayId.id()).toObject());
        uint32_t len = nobj->getDenseInitializedLength();
        if (len == 0) {
          FAIL_IC();
        }
        ObjectElements* elems = nobj->getElementsHeader();
        double accum = 0;
        for (uint32_t i = 0; i < len; i++) {
          HeapSlot* slot = &elems->elements()[i];
          Value val = slot->get();
          if (!val.isNumber()) {
            FAIL_IC();
          }
          double valDouble = val.toNumber();
          if (i > 0) {
            accum = DoubleMinMax(isMax, accum, valDouble);
          } else {
            accum = valDouble;
          }
        }
        retValue = DoubleValue(accum).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MathFunctionNumberResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        UnaryMathFunction fun = cacheIRReader.unaryMathFunction();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        auto funPtr = GetUnaryMathFunctionPtr(fun);
        retValue = DoubleValue(funPtr(input)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NumberParseIntResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId radixId = cacheIRReader.int32OperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        int32_t radix = int32_t(READ_REG(radixId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<Value> result(&ctx.state.value0);
          if (!NumberParseInt(cx, str0, radix, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = result.asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(DoubleParseIntResult) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        if (std::isnan(input)) {
          FAIL_IC();
        }
        int32_t result = int32_t(input);
        if (double(result) != input) {
          FAIL_IC();
        }
        retValue = Int32Value(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardTagNotEqual) {
        ValueTagOperandId lhsId = cacheIRReader.valueTagOperandId();
        ValueTagOperandId rhsId = cacheIRReader.valueTagOperandId();
        int32_t lhs = int32_t(READ_REG(lhsId.id()));
        int32_t rhs = int32_t(READ_REG(rhsId.id()));
        if (lhs == rhs) {
          FAIL_IC();
        }
        if (JSValueTag(lhs) <= JSVAL_TAG_INT32 ||
            JSValueTag(rhs) <= JSVAL_TAG_INT32) {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GuardNumberToIntPtrIndex) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        bool supportOOB = cacheIRReader.readBool();
        (void)supportOOB;
        IntPtrOperandId resultId = cacheIRReader.intPtrOperandId();
        BOUNDSCHECK(resultId);
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        // For simplicity, support only uint32 range for now. This
        // covers 32-bit and 64-bit systems.
        if (input < 0.0 || input >= (uint64_t(1) << 32)) {
          FAIL_IC();
        }
        uintptr_t result = static_cast<uintptr_t>(input);
        // Convert back and compare to detect rounded fractional
        // parts.
        if (static_cast<double>(result) != input) {
          FAIL_IC();
        }
        WRITE_REG(resultId.id(), uint64_t(result), OBJECT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadTypeOfObjectResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        const JSClass* cls = obj->getClass();
        if (cls->isProxyObject()) {
          FAIL_IC();
        }
        if (obj->is<JSFunction>()) {
          retValue =
              StringValue(ctx.frameMgr.cxForLocalUseOnly()->names().function)
                  .asRawBits();
        } else if (cls->emulatesUndefined()) {
          retValue =
              StringValue(ctx.frameMgr.cxForLocalUseOnly()->names().undefined)
                  .asRawBits();
        } else {
          retValue =
              StringValue(ctx.frameMgr.cxForLocalUseOnly()->names().object)
                  .asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(PackedArrayPopResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayObject* aobj =
            reinterpret_cast<ArrayObject*>(READ_REG(objId.id()));
        ObjectElements* elements = aobj->getElementsHeader();
        if (!elements->isPacked() || elements->hasNonwritableArrayLength() ||
            elements->isNotExtensible() || elements->maybeInIteration()) {
          FAIL_IC();
        }
        size_t len = aobj->length();
        if (len != aobj->getDenseInitializedLength()) {
          FAIL_IC();
        }
        if (len == 0) {
          retValue = UndefinedValue().asRawBits();
        } else {
          HeapSlot* slot = &elements->elements()[len - 1];
          retValue = slot->get().asRawBits();
          len--;
          aobj->setDenseInitializedLength(len);
          aobj->setLengthToInitializedLength();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(PackedArrayShiftResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayObject* aobj =
            reinterpret_cast<ArrayObject*>(READ_REG(objId.id()));
        ObjectElements* elements = aobj->getElementsHeader();
        if (!elements->isPacked() || elements->hasNonwritableArrayLength() ||
            elements->isNotExtensible() || elements->maybeInIteration()) {
          FAIL_IC();
        }
        size_t len = aobj->length();
        if (len != aobj->getDenseInitializedLength()) {
          FAIL_IC();
        }
        if (len == 0) {
          retValue = UndefinedValue().asRawBits();
        } else {
          HeapSlot* slot = &elements->elements()[0];
          retValue = slot->get().asRawBits();
          ArrayShiftMoveElements(aobj);
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(PackedArraySliceResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        ObjOperandId arrayId = cacheIRReader.objOperandId();
        Int32OperandId beginId = cacheIRReader.int32OperandId();
        Int32OperandId endId = cacheIRReader.int32OperandId();
        (void)templateObjectOffset;
        ArrayObject* aobj =
            reinterpret_cast<ArrayObject*>(READ_REG(arrayId.id()));
        int32_t begin = int32_t(READ_REG(beginId.id()));
        int32_t end = int32_t(READ_REG(endId.id()));
        if (!aobj->getElementsHeader()->isPacked()) {
          FAIL_IC();
        }
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> arr(&ctx.state.obj0, aobj);
          JSObject* ret = ArraySliceDense(cx, arr, begin, end, nullptr);
          if (!ret) {
            FAIL_IC();
          }
          retValue = ObjectValue(*ret).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsPackedArrayResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (!obj->is<ArrayObject>()) {
          retValue = BooleanValue(false).asRawBits();
          PREDICT_RETURN();
          DISPATCH_CACHEOP();
        }
        ArrayObject* aobj =
            reinterpret_cast<ArrayObject*>(READ_REG(objId.id()));
        if (aobj->length() != aobj->getDenseInitializedLength()) {
          retValue = BooleanValue(false).asRawBits();
          PREDICT_RETURN();
          DISPATCH_CACHEOP();
        }
        retValue = BooleanValue(aobj->denseElementsArePacked()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArgumentsObjectLengthResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArgumentsObject* obj =
            reinterpret_cast<ArgumentsObject*>(READ_REG(objId.id()));
        if (obj->hasOverriddenLength()) {
          FAIL_IC();
        }
        retValue = Int32Value(obj->initialLength()).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadArgumentsObjectLength) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        ArgumentsObject* obj =
            reinterpret_cast<ArgumentsObject*>(READ_REG(objId.id()));
        if (obj->hasOverriddenLength()) {
          FAIL_IC();
        }
        WRITE_REG(resultId.id(), obj->initialLength(), INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ObjectToIteratorResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t enumeratorsAddr = cacheIRReader.stubOffset();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        (void)enumeratorsAddr;
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> rootedObj(&ctx.state.obj0, obj);
          auto* iter = GetIterator(cx, rootedObj);
          if (!iter) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*iter).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadUndefinedResult) {
        retValue = UndefinedValue().asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadDoubleConstant) {
        uint32_t valOffset = cacheIRReader.stubOffset();
        NumberOperandId resultId = cacheIRReader.numberOperandId();
        BOUNDSCHECK(resultId);
        WRITE_VALUE_REG(
            resultId.id(),
            Value::fromRawBits(stubInfo->getStubRawInt64(cstub, valOffset)));
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadBooleanConstant) {
        bool val = cacheIRReader.readBool();
        BooleanOperandId resultId = cacheIRReader.booleanOperandId();
        BOUNDSCHECK(resultId);
        WRITE_REG(resultId.id(), val ? 1 : 0, BOOLEAN);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadUndefined) {
        ValOperandId resultId = cacheIRReader.numberOperandId();
        BOUNDSCHECK(resultId);
        WRITE_VALUE_REG(resultId.id(), UndefinedValue());
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadConstantString) {
        uint32_t valOffset = cacheIRReader.stubOffset();
        StringOperandId resultId = cacheIRReader.stringOperandId();
        BOUNDSCHECK(resultId);
        JSString* str = reinterpret_cast<JSString*>(
            stubInfo->getStubRawWord(cstub, valOffset));
        WRITE_REG(resultId.id(), reinterpret_cast<uint64_t>(str), STRING);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewPlainObjectResult) {
        uint32_t numFixedSlots = cacheIRReader.uint32Immediate();
        uint32_t numDynamicSlots = cacheIRReader.uint32Immediate();
        gc::AllocKind allocKind = cacheIRReader.allocKind();
        uint32_t shapeOffset = cacheIRReader.stubOffset();
        uint32_t siteOffset = cacheIRReader.stubOffset();
        (void)numFixedSlots;
        (void)numDynamicSlots;
        SharedShape* shape = reinterpret_cast<SharedShape*>(
            stubInfo->getStubRawWord(cstub, shapeOffset));
        gc::AllocSite* site = reinterpret_cast<gc::AllocSite*>(
            stubInfo->getStubRawWord(cstub, siteOffset));
        {
          PUSH_IC_FRAME();
          Rooted<SharedShape*> rootedShape(cx, shape);
          auto* result =
              NewPlainObjectBaselineFallback(cx, rootedShape, allocKind, site);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewArrayObjectResult) {
        uint32_t arrayLength = cacheIRReader.uint32Immediate();
        uint32_t shapeOffset = cacheIRReader.stubOffset();
        uint32_t siteOffset = cacheIRReader.stubOffset();
        (void)shapeOffset;
        gc::AllocSite* site = reinterpret_cast<gc::AllocSite*>(
            stubInfo->getStubRawWord(cstub, siteOffset));
        gc::AllocKind allocKind = GuessArrayGCKind(arrayLength);
        MOZ_ASSERT(gc::GetObjectFinalizeKind(&ArrayObject::class_) ==
                   gc::FinalizeKind::None);
        MOZ_ASSERT(!IsFinalizedKind(allocKind));
        {
          PUSH_IC_FRAME();
          auto* result =
              NewArrayObjectBaselineFallback(cx, arrayLength, allocKind, site);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewArrayFromLengthResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        Int32OperandId lengthId = cacheIRReader.int32OperandId();
        uint32_t siteOffset = cacheIRReader.stubOffset();
        ArrayObject* templateObject = reinterpret_cast<ArrayObject*>(
            stubInfo->getStubRawWord(cstub, templateObjectOffset));
        gc::AllocSite* site = reinterpret_cast<gc::AllocSite*>(
            stubInfo->getStubRawWord(cstub, siteOffset));
        int32_t length = int32_t(READ_REG(lengthId.id()));
        {
          PUSH_IC_FRAME();
          Rooted<ArrayObject*> templateObjectRooted(cx, templateObject);
          auto* result =
              ArrayConstructorOneArg(cx, templateObjectRooted, length, site);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewTypedArrayFromLengthResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        Int32OperandId lengthId = cacheIRReader.int32OperandId();
        TypedArrayObject* templateObject = reinterpret_cast<TypedArrayObject*>(
            stubInfo->getStubRawWord(cstub, templateObjectOffset));
        int32_t length = int32_t(READ_REG(lengthId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> templateObjectRooted(&ctx.state.obj0,
                                                         templateObject);
          auto* result = NewTypedArrayWithTemplateAndLength(
              cx, templateObjectRooted, length);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewTypedArrayFromArrayBufferResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        ObjOperandId bufferId = cacheIRReader.objOperandId();
        ValOperandId byteOffsetId = cacheIRReader.valOperandId();
        ValOperandId lengthId = cacheIRReader.valOperandId();
        TypedArrayObject* templateObject = reinterpret_cast<TypedArrayObject*>(
            stubInfo->getStubRawWord(cstub, templateObjectOffset));
        JSObject* buffer = reinterpret_cast<JSObject*>(READ_REG(bufferId.id()));
        Value byteOffset = READ_VALUE_REG(byteOffsetId.id());
        Value length = READ_VALUE_REG(lengthId.id());
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> templateObjectRooted(&ctx.state.obj0,
                                                         templateObject);
          ReservedRooted<JSObject*> bufferRooted(&ctx.state.obj1, buffer);
          ReservedRooted<Value> byteOffsetRooted(&ctx.state.value0, byteOffset);
          ReservedRooted<Value> lengthRooted(&ctx.state.value1, length);
          auto* result = NewTypedArrayWithTemplateAndBuffer(
              cx, templateObjectRooted, bufferRooted, byteOffsetRooted,
              lengthRooted);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewTypedArrayFromArrayResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        ObjOperandId arrayId = cacheIRReader.objOperandId();
        TypedArrayObject* templateObject = reinterpret_cast<TypedArrayObject*>(
            stubInfo->getStubRawWord(cstub, templateObjectOffset));
        JSObject* array = reinterpret_cast<JSObject*>(READ_REG(arrayId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> templateObjectRooted(&ctx.state.obj0,
                                                         templateObject);
          ReservedRooted<JSObject*> arrayRooted(&ctx.state.obj1, array);
          auto* result = NewTypedArrayWithTemplateAndArray(
              cx, templateObjectRooted, arrayRooted);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ObjectToStringResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        {
          PUSH_IC_FRAME();
          auto* result = ObjectClassToString(cx, obj);
          if (!result) {
            FAIL_IC();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallNativeGetterResult) {
        ValOperandId receiverId = cacheIRReader.valOperandId();
        uint32_t getterOffset = cacheIRReader.stubOffset();
        bool sameRealm = cacheIRReader.readBool();
        uint32_t nargsAndFlagsOffset = cacheIRReader.stubOffset();
        (void)sameRealm;
        (void)nargsAndFlagsOffset;
        Value receiver = READ_VALUE_REG(receiverId.id());
        JSFunction* getter = reinterpret_cast<JSFunction*>(
            stubInfo->getStubRawWord(cstub, getterOffset));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSFunction*> getterRooted(&ctx.state.fun0, getter);
          ReservedRooted<Value> receiverRooted(&ctx.state.value0, receiver);
          ReservedRooted<Value> resultRooted(&ctx.state.value1);
          if (!CallNativeGetter(cx, getterRooted, receiverRooted,
                                &resultRooted)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = resultRooted.asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallNativeSetter) {
        ValOperandId receiverId = cacheIRReader.valOperandId();
        uint32_t setterOffset = cacheIRReader.stubOffset();
        ObjOperandId rhsId = cacheIRReader.objOperandId();
        bool sameRealm = cacheIRReader.readBool();
        uint32_t nargsAndFlagsOffset = cacheIRReader.stubOffset();
        (void)sameRealm;
        (void)nargsAndFlagsOffset;
        JSObject* receiver =
            reinterpret_cast<JSObject*>(READ_REG(receiverId.id()));
        Value rhs = READ_VALUE_REG(rhsId.id());
        JSFunction* setter = reinterpret_cast<JSFunction*>(
            stubInfo->getStubRawWord(cstub, setterOffset));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSFunction*> setterRooted(&ctx.state.fun0, setter);
          ReservedRooted<JSObject*> receiverRooted(&ctx.state.obj0, receiver);
          ReservedRooted<Value> rhsRooted(&ctx.state.value1, rhs);
          if (!CallNativeSetter(cx, setterRooted, receiverRooted, rhsRooted)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadInstanceOfObjectResult) {
        ValOperandId lhsId = cacheIRReader.valOperandId();
        ObjOperandId protoId = cacheIRReader.objOperandId();
        Value lhs = READ_VALUE_REG(lhsId.id());
        JSObject* rhsProto =
            reinterpret_cast<JSObject*>(READ_REG(protoId.id()));
        if (!lhs.isObject()) {
          retValue = BooleanValue(false).asRawBits();
          PREDICT_RETURN();
          DISPATCH_CACHEOP();
        }

        JSObject* lhsObj = &lhs.toObject();
        bool result = false;
        while (true) {
          TaggedProto proto = lhsObj->taggedProto();
          if (proto.isDynamic()) {
            FAIL_IC();
          }
          JSObject* protoObj = proto.toObjectOrNull();
          if (!protoObj) {
            result = false;
            break;
          }
          if (protoObj == rhsProto) {
            result = true;
            break;
          }
          lhsObj = protoObj;
        }
        retValue = BooleanValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringFromCharCodeResult) {
        Int32OperandId codeId = cacheIRReader.int32OperandId();
        uint32_t code = uint32_t(READ_REG(codeId.id()));
        StaticStrings& sstr = ctx.frameMgr.cxForLocalUseOnly()->staticStrings();
        if (sstr.hasUnit(code)) {
          retValue = StringValue(sstr.getUnit(code)).asRawBits();
        } else {
          PUSH_IC_FRAME();
          auto* result = StringFromCharCode(cx, code);
          if (!result) {
            FAIL_IC();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringFromCodePointResult) {
        Int32OperandId codeId = cacheIRReader.int32OperandId();
        uint32_t code = uint32_t(READ_REG(codeId.id()));
        if (code > unicode::NonBMPMax) {
          FAIL_IC();
        }
        StaticStrings& sstr = ctx.frameMgr.cxForLocalUseOnly()->staticStrings();
        if (sstr.hasUnit(code)) {
          retValue = StringValue(sstr.getUnit(code)).asRawBits();
        } else {
          PUSH_IC_FRAME();
          auto* result = StringFromCodePoint(cx, code);
          if (!result) {
            FAIL_IC();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringIncludesResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId searchStrId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* searchStr =
            reinterpret_cast<JSString*>(READ_REG(searchStrId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, searchStr);
          bool result = false;
          if (!StringIncludes(cx, str0, str1, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = BooleanValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringIndexOfResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId searchStrId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* searchStr =
            reinterpret_cast<JSString*>(READ_REG(searchStrId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, searchStr);
          int32_t result = 0;
          if (!StringIndexOf(cx, str0, str1, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = Int32Value(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringLastIndexOfResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId searchStrId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* searchStr =
            reinterpret_cast<JSString*>(READ_REG(searchStrId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, searchStr);
          int32_t result = 0;
          if (!StringLastIndexOf(cx, str0, str1, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = Int32Value(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringStartsWithResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId searchStrId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* searchStr =
            reinterpret_cast<JSString*>(READ_REG(searchStrId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, searchStr);
          bool result = false;
          if (!StringStartsWith(cx, str0, str1, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = BooleanValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringEndsWithResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId searchStrId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* searchStr =
            reinterpret_cast<JSString*>(READ_REG(searchStrId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, searchStr);
          bool result = false;
          if (!StringEndsWith(cx, str0, str1, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = BooleanValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringToLowerCaseResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        {
          PUSH_IC_FRAME();
          auto* result = StringToLowerCase(cx, str);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringToUpperCaseResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        {
          PUSH_IC_FRAME();
          auto* result = StringToUpperCase(cx, str);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringTrimResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          auto* result = StringTrim(cx, str0);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringTrimStartResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          auto* result = StringTrimStart(cx, str0);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringTrimEndResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          auto* result = StringTrimEnd(cx, str0);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallSubstringKernelResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        Int32OperandId beginId = cacheIRReader.int32OperandId();
        Int32OperandId lengthId = cacheIRReader.int32OperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        int32_t begin = int32_t(READ_REG(beginId.id()));
        int32_t length = int32_t(READ_REG(lengthId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          auto* result = SubstringKernel(cx, str0, begin, length);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringReplaceStringResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId patternId = cacheIRReader.stringOperandId();
        StringOperandId replacementId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* pattern =
            reinterpret_cast<JSString*>(READ_REG(patternId.id()));
        JSString* replacement =
            reinterpret_cast<JSString*>(READ_REG(replacementId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, pattern);
          ReservedRooted<JSString*> str2(&ctx.state.str2, replacement);
          auto* result = StringReplace(cx, str0, str1, str2);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringSplitStringResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        StringOperandId separatorId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSString* separator =
            reinterpret_cast<JSString*>(READ_REG(separatorId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSString*> str0(&ctx.state.str0, str);
          ReservedRooted<JSString*> str1(&ctx.state.str1, separator);
          auto* result = StringSplitString(cx, str0, str1, INT32_MAX);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(StringToAtom) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        JSAtom* result =
            AtomizeStringNoGC(ctx.frameMgr.cxForLocalUseOnly(), str);
        if (!result) {
          FAIL_IC();
        }
        WRITE_REG(strId.id(), reinterpret_cast<uint64_t>(result), STRING);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IdToStringOrSymbol) {
        ValOperandId resultId = cacheIRReader.valOperandId();
        ValOperandId idId = cacheIRReader.valOperandId();
        BOUNDSCHECK(resultId);
        Value id = READ_VALUE_REG(idId.id());
        if (id.isString() || id.isSymbol()) {
          WRITE_VALUE_REG(resultId.id(), id);
        } else if (id.isInt32()) {
          int32_t idInt = id.toInt32();
          StaticStrings& sstr =
              ctx.frameMgr.cxForLocalUseOnly()->staticStrings();
          if (sstr.hasInt(idInt)) {
            WRITE_VALUE_REG(resultId.id(), StringValue(sstr.getInt(idInt)));
          } else {
            PUSH_IC_FRAME();
            auto* result = Int32ToStringPure(cx, idInt);
            if (!result) {
              ctx.error = PBIResult::Error;
              return IC_ERROR_SENTINEL();
            }
            WRITE_VALUE_REG(resultId.id(), StringValue(result));
          }
        } else {
          FAIL_IC();
        }
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewStringIteratorResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        (void)templateObjectOffset;
        {
          PUSH_IC_FRAME();
          auto* result = NewStringIterator(cx);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsArrayResult) {
        ValOperandId valId = cacheIRReader.valOperandId();
        Value val = READ_VALUE_REG(valId.id());
        if (!val.isObject()) {
          retValue = BooleanValue(false).asRawBits();
        } else {
          JSObject* obj = &val.toObject();
          if (obj->getClass()->isProxyObject()) {
            FAIL_IC();
          }
          retValue = BooleanValue(obj->is<ArrayObject>()).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsCallableResult) {
        ValOperandId valId = cacheIRReader.valOperandId();
        Value val = READ_VALUE_REG(valId.id());
        if (!val.isObject()) {
          retValue = BooleanValue(false).asRawBits();
        } else {
          JSObject* obj = &val.toObject();
          if (obj->getClass()->isProxyObject()) {
            FAIL_IC();
          }
          bool callable =
              obj->is<JSFunction>() || obj->getClass()->getCall() != nullptr;
          retValue = BooleanValue(callable).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsConstructorResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        bool ctor = obj->isConstructor();
        retValue = BooleanValue(ctor).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsCrossRealmArrayConstructorResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        bool result =
            obj->shape()->realm() !=
                ctx.frameMgr.cxForLocalUseOnly()->realm() &&
            obj->is<JSFunction>() && obj->as<JSFunction>().isNativeFun() &&
            obj->as<JSFunction>().nativeUnchecked() == &js::ArrayConstructor;
        retValue = BooleanValue(result).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsTypedArrayResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        bool isPossiblyWrapped = cacheIRReader.readBool();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (IsTypedArrayClass(obj->getClass())) {
          retValue = BooleanValue(true).asRawBits();
        } else if (isPossiblyWrapped && obj->is<WrapperObject>()) {
          PUSH_IC_FRAME();
          bool result;
          if (!IsPossiblyWrappedTypedArray(cx, obj, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = BooleanValue(result).asRawBits();
        } else {
          retValue = BooleanValue(false).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IsTypedArrayConstructorResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        retValue = BooleanValue(IsTypedArrayConstructor(obj)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ArrayBufferViewByteOffsetInt32Result) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayBufferViewObject* abvo =
            reinterpret_cast<ArrayBufferViewObject*>(READ_REG(objId.id()));
        size_t byteOffset =
            size_t(abvo->getFixedSlot(ArrayBufferViewObject::BYTEOFFSET_SLOT)
                       .toPrivate());
        if (byteOffset > size_t(INT32_MAX)) {
          FAIL_IC();
        }
        retValue = Int32Value(int32_t(byteOffset)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ArrayBufferViewByteOffsetDoubleResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ArrayBufferViewObject* abvo =
            reinterpret_cast<ArrayBufferViewObject*>(READ_REG(objId.id()));
        size_t byteOffset =
            size_t(abvo->getFixedSlot(ArrayBufferViewObject::BYTEOFFSET_SLOT)
                       .toPrivate());
        retValue = DoubleValue(double(byteOffset)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(TypedArrayByteLengthInt32Result) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        TypedArrayObject* tao =
            reinterpret_cast<TypedArrayObject*>(READ_REG(objId.id()));
        if (!tao->length()) {
          FAIL_IC();
        }
        size_t length = *tao->length() * tao->bytesPerElement();
        if (length > size_t(INT32_MAX)) {
          FAIL_IC();
        }
        retValue = Int32Value(int32_t(length)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(TypedArrayByteLengthDoubleResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        TypedArrayObject* tao =
            reinterpret_cast<TypedArrayObject*>(READ_REG(objId.id()));
        if (!tao->length()) {
          FAIL_IC();
        }
        size_t length = *tao->length() * tao->bytesPerElement();
        retValue = DoubleValue(double(length)).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(TypedArrayElementSizeResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        TypedArrayObject* tao =
            reinterpret_cast<TypedArrayObject*>(READ_REG(objId.id()));
        retValue = Int32Value(int32_t(tao->bytesPerElement())).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MegamorphicStoreSlot) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        uint32_t nameOffset = cacheIRReader.stubOffset();
        ValOperandId valId = cacheIRReader.valOperandId();
        bool strict = cacheIRReader.readBool();

        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        jsid id =
            jsid::fromRawBits(stubInfo->getStubRawWord(cstub, nameOffset));
        Value val = READ_VALUE_REG(valId.id());

        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> objRooted(&ctx.state.obj0, obj);
          ReservedRooted<jsid> idRooted(&ctx.state.id0, id);
          ReservedRooted<Value> valRooted(&ctx.state.value0, val);
          if (!SetPropertyMegamorphic<false>(cx, objRooted, idRooted, valRooted,
                                             strict)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
        }

        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(MegamorphicHasPropResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ValOperandId valId = cacheIRReader.valOperandId();
        bool hasOwn = cacheIRReader.readBool();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        if (!obj->is<NativeObject>()) {
          FAIL_IC();
        }
        Value val[2] = {READ_VALUE_REG(valId.id()), UndefinedValue()};
        {
          PUSH_IC_FRAME();
          bool ok =
              hasOwn
                  ? HasNativeDataPropertyPure<true>(cx, obj, nullptr, &val[0])
                  : HasNativeDataPropertyPure<false>(cx, obj, nullptr, &val[0]);
          if (!ok) {
            FAIL_IC();
          }
          retValue = val[1].asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ArrayJoinResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        StringOperandId sepId = cacheIRReader.stringOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        JSString* sep = reinterpret_cast<JSString*>(READ_REG(sepId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> obj0(&ctx.state.obj0, obj);
          ReservedRooted<JSString*> str0(&ctx.state.str0, sep);
          auto* result = ArrayJoin(cx, obj0, str0);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallSetArrayLength) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        bool strict = cacheIRReader.readBool();
        ValOperandId rhsId = cacheIRReader.valOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        Value rhs = READ_VALUE_REG(rhsId.id());
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> obj0(&ctx.state.obj0, obj);
          ReservedRooted<Value> value0(&ctx.state.value0, rhs);
          if (!SetArrayLength(cx, obj0, value0, strict)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ObjectKeysResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        JSObject* obj = reinterpret_cast<JSObject*>(READ_REG(objId.id()));
        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> obj0(&ctx.state.obj0, obj);
          auto* result = ObjectKeys(cx, obj0);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(ObjectCreateResult) {
        uint32_t templateOffset = cacheIRReader.stubOffset();
        PlainObject* templateObj = reinterpret_cast<PlainObject*>(
            stubInfo->getStubRawWord(cstub, templateOffset));
        {
          PUSH_IC_FRAME();
          Rooted<PlainObject*> templateRooted(cx, templateObj);
          auto* result = ObjectCreateWithTemplate(cx, templateRooted);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallNumberToString) {
        NumberOperandId inputId = cacheIRReader.numberOperandId();
        StringOperandId resultId = cacheIRReader.stringOperandId();
        BOUNDSCHECK(resultId);
        double input = READ_VALUE_REG(inputId.id()).toNumber();
        {
          PUSH_IC_FRAME();
          auto* result = NumberToStringPure(cx, input);
          if (!result) {
            FAIL_IC();
          }
          WRITE_REG(resultId.id(), reinterpret_cast<uint64_t>(result), STRING);
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(Int32ToStringWithBaseResult) {
        Int32OperandId inputId = cacheIRReader.int32OperandId();
        Int32OperandId baseId = cacheIRReader.int32OperandId();
        int32_t input = int32_t(READ_REG(inputId.id()));
        int32_t base = int32_t(READ_REG(baseId.id()));
        if (base < 2 || base > 36) {
          FAIL_IC();
        }
        {
          PUSH_IC_FRAME();
          auto* result = Int32ToStringWithBase<CanGC>(cx, input, base, true);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = StringValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(BooleanToString) {
        BooleanOperandId inputId = cacheIRReader.booleanOperandId();
        StringOperandId resultId = cacheIRReader.stringOperandId();
        BOUNDSCHECK(resultId);
        bool input = READ_REG(inputId.id()) != 0;
        auto& names = ctx.frameMgr.cxForLocalUseOnly()->names();
        JSString* result = input ? names.true_ : names.false_;
        WRITE_REG(resultId.id(), reinterpret_cast<uint64_t>(result), STRING);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(IndirectTruncateInt32Result) {
        Int32OperandId valId = cacheIRReader.int32OperandId();
        int32_t value = int32_t(READ_REG(valId.id()));
        retValue = Int32Value(value).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(GetFirstDollarIndexResult) {
        StringOperandId strId = cacheIRReader.stringOperandId();
        JSString* str = reinterpret_cast<JSString*>(READ_REG(strId.id()));
        int32_t result = 0;
        {
          PUSH_IC_FRAME();
          if (!GetFirstDollarIndexRaw(cx, str, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = Int32Value(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadBoundFunctionNumArgs) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId resultId = cacheIRReader.int32OperandId();
        BOUNDSCHECK(resultId);
        BoundFunctionObject* obj =
            reinterpret_cast<BoundFunctionObject*>(READ_REG(objId.id()));
        WRITE_REG(resultId.id(), obj->numBoundArgs(), INT32);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(LoadBoundFunctionTarget) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        ObjOperandId resultId = cacheIRReader.objOperandId();
        BOUNDSCHECK(resultId);
        BoundFunctionObject* obj =
            reinterpret_cast<BoundFunctionObject*>(READ_REG(objId.id()));
        WRITE_REG(resultId.id(), reinterpret_cast<uint64_t>(obj->getTarget()),
                  OBJECT);
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(BindFunctionResult)
      CACHEOP_CASE_FALLTHROUGH(SpecializedBindFunctionResult) {
        ObjOperandId targetId = cacheIRReader.objOperandId();
        uint32_t argc = cacheIRReader.uint32Immediate();
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();

        JSObject* target = reinterpret_cast<JSObject*>(READ_REG(targetId.id()));
        BoundFunctionObject* templateObject =
            (cacheop == CacheOp::SpecializedBindFunctionResult)
                ? reinterpret_cast<BoundFunctionObject*>(
                      stubInfo->getStubRawWord(cstub, templateObjectOffset))
                : nullptr;

        StackVal* origArgs = ctx.sp();
        {
          PUSH_IC_FRAME();

          for (uint32_t i = 0; i < argc; i++) {
            PUSH(origArgs[i]);
          }
          Value* args = reinterpret_cast<Value*>(sp);

          ReservedRooted<JSObject*> targetRooted(&ctx.state.obj0, target);
          BoundFunctionObject* result;
          if (cacheop == CacheOp::BindFunctionResult) {
            result = BoundFunctionObject::functionBindImpl(cx, targetRooted,
                                                           args, argc, nullptr);
          } else {
            Rooted<BoundFunctionObject*> templateObjectRooted(cx,
                                                              templateObject);
            result = BoundFunctionObject::functionBindSpecializedBaseline(
                cx, targetRooted, args, argc, templateObjectRooted);
          }
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }

          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallRegExpMatcherResult)
      CACHEOP_CASE_FALLTHROUGH(CallRegExpSearcherResult) {
        ObjOperandId regexpId = cacheIRReader.objOperandId();
        StringOperandId inputId = cacheIRReader.stringOperandId();
        Int32OperandId lastIndexId = cacheIRReader.int32OperandId();
        uint32_t stubOffset = cacheIRReader.stubOffset();

        JSObject* regexp = reinterpret_cast<JSObject*>(READ_REG(regexpId.id()));
        JSString* input = reinterpret_cast<JSString*>(READ_REG(inputId.id()));
        int32_t lastIndex = int32_t(READ_REG(lastIndexId.id()));
        (void)stubOffset;

        {
          PUSH_IC_FRAME();
          ReservedRooted<JSObject*> regexpRooted(&ctx.state.obj0, regexp);
          ReservedRooted<JSString*> inputRooted(&ctx.state.str0, input);

          if (cacheop == CacheOp::CallRegExpMatcherResult) {
            ReservedRooted<Value> result(&ctx.state.value0, UndefinedValue());
            if (!RegExpMatcherRaw(cx, regexpRooted, inputRooted, lastIndex,
                                  nullptr, &result)) {
              ctx.error = PBIResult::Error;
              return IC_ERROR_SENTINEL();
            }
            retValue = result.asRawBits();
          } else {
            int32_t result = 0;
            if (!RegExpSearcherRaw(cx, regexpRooted, inputRooted, lastIndex,
                                   nullptr, &result)) {
              ctx.error = PBIResult::Error;
              return IC_ERROR_SENTINEL();
            }
            retValue = Int32Value(result).asRawBits();
          }
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(RegExpSearcherLastLimitResult) {
        uint32_t lastLimit =
            ctx.frameMgr.cxForLocalUseOnly()->regExpSearcherLastLimit;
        retValue = Int32Value(lastLimit).asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(RegExpHasCaptureGroupsResult) {
        ObjOperandId regexpId = cacheIRReader.objOperandId();
        StringOperandId inputId = cacheIRReader.stringOperandId();
        RegExpObject* regexp =
            reinterpret_cast<RegExpObject*>(READ_REG(regexpId.id()));
        JSString* input = reinterpret_cast<JSString*>(READ_REG(inputId.id()));
        {
          PUSH_IC_FRAME();
          Rooted<RegExpObject*> regexpRooted(cx, regexp);
          ReservedRooted<JSString*> inputRooted(&ctx.state.str0, input);
          bool result = false;
          if (!RegExpHasCaptureGroups(cx, regexpRooted, inputRooted, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = BooleanValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(RegExpBuiltinExecMatchResult) {
        ObjOperandId regexpId = cacheIRReader.objOperandId();
        StringOperandId inputId = cacheIRReader.stringOperandId();
        uint32_t stubOffset = cacheIRReader.stubOffset();

        RegExpObject* regexp =
            reinterpret_cast<RegExpObject*>(READ_REG(regexpId.id()));
        JSString* input = reinterpret_cast<JSString*>(READ_REG(inputId.id()));
        (void)stubOffset;

        {
          PUSH_IC_FRAME();
          Rooted<RegExpObject*> regexpRooted(cx, regexp);
          ReservedRooted<JSString*> inputRooted(&ctx.state.str0, input);
          ReservedRooted<Value> output(&ctx.state.value0, UndefinedValue());
          if (!RegExpBuiltinExecMatchFromJit(cx, regexpRooted, inputRooted,
                                             nullptr, &output)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = output.asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(RegExpBuiltinExecTestResult) {
        ObjOperandId regexpId = cacheIRReader.objOperandId();
        StringOperandId inputId = cacheIRReader.stringOperandId();
        uint32_t stubOffset = cacheIRReader.stubOffset();

        RegExpObject* regexp =
            reinterpret_cast<RegExpObject*>(READ_REG(regexpId.id()));
        JSString* input = reinterpret_cast<JSString*>(READ_REG(inputId.id()));
        (void)stubOffset;

        {
          PUSH_IC_FRAME();
          Rooted<RegExpObject*> regexpRooted(cx, regexp);
          ReservedRooted<JSString*> inputRooted(&ctx.state.str0, input);
          bool result = false;
          if (!RegExpBuiltinExecTestFromJit(cx, regexpRooted, inputRooted,
                                            &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = BooleanValue(result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(RegExpFlagResult) {
        ObjOperandId regexpId = cacheIRReader.objOperandId();
        uint32_t flagsMask = cacheIRReader.uint32Immediate();
        RegExpObject* regexp =
            reinterpret_cast<RegExpObject*>(READ_REG(regexpId.id()));
        JS::RegExpFlags flags = regexp->getFlags();
        retValue = BooleanValue((uint32_t(flags.value()) & flagsMask) != 0)
                       .asRawBits();
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(NewRegExpStringIteratorResult) {
        uint32_t templateObjectOffset = cacheIRReader.stubOffset();
        (void)templateObjectOffset;
        {
          PUSH_IC_FRAME();
          auto* result = NewRegExpStringIterator(cx);
          if (!result) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = ObjectValue(*result).asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

      CACHEOP_CASE(CallGetSparseElementResult) {
        ObjOperandId objId = cacheIRReader.objOperandId();
        Int32OperandId indexId = cacheIRReader.int32OperandId();
        NativeObject* nobj =
            reinterpret_cast<NativeObject*>(READ_REG(objId.id()));
        int32_t index = int32_t(READ_REG(indexId.id()));
        {
          PUSH_IC_FRAME();
          Rooted<NativeObject*> nobjRooted(cx, nobj);
          ReservedRooted<Value> result(&ctx.state.value0, UndefinedValue());
          if (!GetSparseElementHelper(cx, nobjRooted, index, &result)) {
            ctx.error = PBIResult::Error;
            return IC_ERROR_SENTINEL();
          }
          retValue = result.asRawBits();
        }
        PREDICT_RETURN();
        DISPATCH_CACHEOP();
      }

#undef PREDICT_NEXT

#ifndef ENABLE_COMPUTED_GOTO_DISPATCH
      default:
        TRACE_PRINTF("unknown CacheOp\n");
        FAIL_IC();
#endif
    }
  }

#define CACHEOP_UNIMPL(name, ...)               \
  cacheop_##name : __attribute__((unused));     \
  TRACE_PRINTF("unknown CacheOp: " #name "\n"); \
  FAIL_IC();
  CACHE_IR_OPS(CACHEOP_UNIMPL)
#undef CACHEOP_UNIMPL

next_ic:
  TRACE_PRINTF("IC failed; next IC\n");
  return CallNextIC(arg0, arg1, stub, ctx);
}

static MOZ_NEVER_INLINE uint64_t CallNextIC(uint64_t arg0, uint64_t arg1,
                                            ICStub* stub, ICCtx& ctx) {
  stub = stub->maybeNext();
  MOZ_ASSERT(stub);
  uint64_t result;
  PBL_CALL_IC(stub->rawJitCode(), ctx, stub, result, arg0, arg1, ctx.arg2,
              true);
  return result;
}

/*
 * -----------------------------------------------
 * IC callsite logic, and fallback stubs
 * -----------------------------------------------
 */

#define DEFINE_IC(kind, arity, fallback_body)                   \
  static uint64_t MOZ_NEVER_INLINE IC##kind##Fallback(          \
      uint64_t arg0, uint64_t arg1, ICStub* stub, ICCtx& ctx) { \
    uint64_t retValue = 0;                                      \
    uint64_t arg2 = ctx.arg2;                                   \
    (void)arg2;                                                 \
    ICFallbackStub* fallback = stub->toFallbackStub();          \
    StackVal* sp = ctx.sp();                                    \
    fallback_body;                                              \
    retValue = ctx.state.res.asRawBits();                       \
    ctx.state.res = UndefinedValue();                           \
    return retValue;                                            \
  error:                                                        \
    ctx.error = PBIResult::Error;                               \
    return IC_ERROR_SENTINEL();                                 \
  }

#define DEFINE_IC_ALIAS(kind, target)                           \
  static uint64_t MOZ_NEVER_INLINE IC##kind##Fallback(          \
      uint64_t arg0, uint64_t arg1, ICStub* stub, ICCtx& ctx) { \
    return IC##target##Fallback(arg0, arg1, stub, ctx);         \
  }

#define IC_LOAD_VAL(state_elem, index)                    \
  ReservedRooted<Value> state_elem(&ctx.state.state_elem, \
                                   Value::fromRawBits(arg##index))
#define IC_LOAD_OBJ(state_elem, index)  \
  ReservedRooted<JSObject*> state_elem( \
      &ctx.state.state_elem, reinterpret_cast<JSObject*>(arg##index))

DEFINE_IC(TypeOf, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoTypeOfFallback(cx, ctx.frame, fallback, value0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(TypeOfEq, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoTypeOfEqFallback(cx, ctx.frame, fallback, value0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetName, 1, {
  IC_LOAD_OBJ(obj0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetNameFallback(cx, ctx.frame, fallback, obj0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(Call, 1, {
  uint32_t argc = uint32_t(arg0);
  uint32_t totalArgs =
      argc + ctx.icregs.extraArgs;  // this, callee, (constructing?), func args
  Value* args = reinterpret_cast<Value*>(&sp[0]);
  TRACE_PRINTF("Call fallback: argc %d totalArgs %d args %p\n", argc, totalArgs,
               args);
  // Reverse values on the stack.
  std::reverse(args, args + totalArgs);
  {
    PUSH_FALLBACK_IC_FRAME();
    if (!DoCallFallback(cx, ctx.frame, fallback, argc, args, &ctx.state.res)) {
      std::reverse(args, args + totalArgs);
      goto error;
    }
  }
});

DEFINE_IC_ALIAS(CallConstructing, Call);

DEFINE_IC(SpreadCall, 1, {
  uint32_t argc = uint32_t(arg0);
  uint32_t totalArgs =
      argc + ctx.icregs.extraArgs;  // this, callee, (constructing?), func args
  Value* args = reinterpret_cast<Value*>(&sp[0]);
  TRACE_PRINTF("Call fallback: argc %d totalArgs %d args %p\n", argc, totalArgs,
               args);
  // Reverse values on the stack.
  std::reverse(args, args + totalArgs);
  {
    PUSH_FALLBACK_IC_FRAME();
    if (!DoSpreadCallFallback(cx, ctx.frame, fallback, args, &ctx.state.res)) {
      std::reverse(args, args + totalArgs);
      goto error;
    }
  }
});

DEFINE_IC_ALIAS(SpreadCallConstructing, SpreadCall);

DEFINE_IC(UnaryArith, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoUnaryArithFallback(cx, ctx.frame, fallback, value0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(BinaryArith, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoBinaryArithFallback(cx, ctx.frame, fallback, value0, value1,
                             &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(ToBool, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoToBoolFallback(cx, ctx.frame, fallback, value0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(Compare, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoCompareFallback(cx, ctx.frame, fallback, value0, value1,
                         &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(InstanceOf, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoInstanceOfFallback(cx, ctx.frame, fallback, value0, value1,
                            &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(In, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoInFallback(cx, ctx.frame, fallback, value0, value1, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(BindName, 1, {
  IC_LOAD_OBJ(obj0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoBindNameFallback(cx, ctx.frame, fallback, obj0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(SetProp, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoSetPropFallback(cx, ctx.frame, fallback, nullptr, value0, value1)) {
    goto error;
  }
});

DEFINE_IC(NewObject, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoNewObjectFallback(cx, ctx.frame, fallback, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetProp, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetPropFallback(cx, ctx.frame, fallback, &value0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetPropSuper, 2, {
  IC_LOAD_VAL(value0, 1);
  IC_LOAD_VAL(value1, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetPropSuperFallback(cx, ctx.frame, fallback, value0, &value1,
                              &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetElem, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetElemFallback(cx, ctx.frame, fallback, value0, value1,
                         &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetElemSuper, 3, {
  IC_LOAD_VAL(value0, 0);  // receiver
  IC_LOAD_VAL(value1, 1);  // obj (lhs)
  IC_LOAD_VAL(value2, 2);  // key (rhs)
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetElemSuperFallback(cx, ctx.frame, fallback, value1, value2, value0,
                              &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetImport, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetImportFallback(cx, ctx.frame, fallback, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(NewArray, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoNewArrayFallback(cx, ctx.frame, fallback, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(Lambda, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoLambdaFallback(cx, ctx.frame, fallback, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(LazyConstant, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoLazyConstantFallback(cx, ctx.frame, fallback, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(SetElem, 3, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  IC_LOAD_VAL(value2, 2);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoSetElemFallback(cx, ctx.frame, fallback, nullptr, value0, value1,
                         value2)) {
    goto error;
  }
});

DEFINE_IC(HasOwn, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoHasOwnFallback(cx, ctx.frame, fallback, value0, value1,
                        &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(CheckPrivateField, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoCheckPrivateFieldFallback(cx, ctx.frame, fallback, value0, value1,
                                   &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(GetIterator, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetIteratorFallback(cx, ctx.frame, fallback, value0, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(ToPropertyKey, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoToPropertyKeyFallback(cx, ctx.frame, fallback, value0,
                               &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(OptimizeSpreadCall, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoOptimizeSpreadCallFallback(cx, ctx.frame, fallback, value0,
                                    &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(OptimizeGetIterator, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoOptimizeGetIteratorFallback(cx, ctx.frame, fallback, value0,
                                     &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(Rest, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoRestFallback(cx, ctx.frame, fallback, &ctx.state.res)) {
    goto error;
  }
});

DEFINE_IC(CloseIter, 1, {
  IC_LOAD_OBJ(obj0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoCloseIterFallback(cx, ctx.frame, fallback, obj0)) {
    goto error;
  }
});

uint8_t* GetPortableFallbackStub(BaselineICFallbackKind kind) {
  switch (kind) {
#define _(ty)                      \
  case BaselineICFallbackKind::ty: \
    return reinterpret_cast<uint8_t*>(&IC##ty##Fallback);
    IC_BASELINE_FALLBACK_CODE_KIND_LIST(_)
#undef _
    case BaselineICFallbackKind::Count:
      MOZ_CRASH("Invalid kind");
  }
}

uint8_t* GetICInterpreter() {
  return reinterpret_cast<uint8_t*>(&ICInterpretOps);
}

/*
 * -----------------------------------------------
 * Main JSOp interpreter
 * -----------------------------------------------
 */

static EnvironmentObject& getEnvironmentFromCoordinate(
    BaselineFrame* frame, EnvironmentCoordinate ec) {
  JSObject* env = frame->environmentChain();
  for (unsigned i = ec.hops(); i; i--) {
    if (env->is<EnvironmentObject>()) {
      env = &env->as<EnvironmentObject>().enclosingEnvironment();
    } else {
      MOZ_ASSERT(env->is<DebugEnvironmentProxy>());
      env = &env->as<DebugEnvironmentProxy>().enclosingEnvironment();
    }
  }
  return env->is<EnvironmentObject>()
             ? env->as<EnvironmentObject>()
             : env->as<DebugEnvironmentProxy>().environment();
}

#ifndef __wasi__
#  define DEBUG_CHECK()                                                   \
    if (frame->isDebuggee()) {                                            \
      TRACE_PRINTF(                                                       \
          "Debug check: frame is debuggee, checking for debug script\n"); \
      if (frame->script()->hasDebugScript()) {                            \
        goto debug;                                                       \
      }                                                                   \
    }
#else
#  define DEBUG_CHECK()
#endif

#define LABEL(op) (&&label_##op)
#ifdef ENABLE_COMPUTED_GOTO_DISPATCH
#  define CASE(op) label_##op:
#  define DISPATCH() \
    DEBUG_CHECK();   \
    goto* addresses[*pc]
#else
#  define CASE(op) label_##op : case JSOp::op:
#  define DISPATCH() \
    DEBUG_CHECK();   \
    goto dispatch
#endif

#define ADVANCE(delta) pc += (delta);
#define ADVANCE_AND_DISPATCH(delta) \
  ADVANCE(delta);                   \
  DISPATCH();

#define END_OP(op) ADVANCE_AND_DISPATCH(JSOpLength_##op);

#define VIRTPUSH(value) PUSH(value)
#define VIRTPOP() POP()
#define VIRTSP(index) sp[(index)]
#define VIRTSPWRITE(index, value) sp[(index)] = (value)
#define SYNCSP()
#define SETLOCAL(i, value) frame->unaliasedLocal(i) = value
#define GETLOCAL(i) frame->unaliasedLocal(i)

#define IC_SET_ARG_FROM_STACK(index, stack_index) \
  ic_arg##index = sp[(stack_index)].asUInt64();
#define IC_POP_ARG(index) ic_arg##index = (*sp++).asUInt64();
#define IC_SET_VAL_ARG(index, expr) ic_arg##index = (expr).asRawBits();
#define IC_SET_OBJ_ARG(index, expr) \
  ic_arg##index = reinterpret_cast<uint64_t>(expr);
#define IC_ZERO_ARG(index) ic_arg##index = 0;
#define IC_PUSH_RESULT() VIRTPUSH(StackVal(ic_ret));

#if !defined(TRACE_INTERP)
#  define PREDICT_NEXT(op)       \
    if (JSOp(*pc) == JSOp::op) { \
      DEBUG_CHECK();             \
      goto label_##op;           \
    }
#else
#  define PREDICT_NEXT(op)
#endif

#ifdef ENABLE_COVERAGE
#  define COUNT_COVERAGE_PC(PC)                                 \
    if (frame->script()->hasScriptCounts()) {                   \
      PCCounts* counts = frame->script()->maybeGetPCCounts(PC); \
      MOZ_ASSERT(counts);                                       \
      counts->numExec()++;                                      \
    }
#  define COUNT_COVERAGE_MAIN()                                        \
    {                                                                  \
      jsbytecode* main = frame->script()->main();                      \
      if (!BytecodeIsJumpTarget(JSOp(*main))) COUNT_COVERAGE_PC(main); \
    }
#else
#  define COUNT_COVERAGE_PC(PC) ;
#  define COUNT_COVERAGE_MAIN() ;
#endif

#define NEXT_IC() icEntry++

#define INVOKE_IC(kind, hasarg2)                                             \
  ctx.sp_ = sp;                                                              \
  frame->interpreterPC() = pc;                                               \
  frame->interpreterICEntry() = icEntry;                                     \
  SYNCSP();                                                                  \
  PBL_CALL_IC(icEntry->firstStub()->rawJitCode(), ctx, icEntry->firstStub(), \
              ic_ret, ic_arg0, ic_arg1, ic_arg2, hasarg2);                   \
  if (ic_ret == IC_ERROR_SENTINEL()) {                                       \
    ic_result = ctx.error;                                                   \
    goto ic_fail;                                                            \
  }                                                                          \
  NEXT_IC();

#define INVOKE_IC_AND_PUSH(kind, hasarg2) \
  INVOKE_IC(kind, hasarg2);               \
  VIRTPUSH(StackVal(ic_ret));

#define VIRTPOPN(n) \
  SYNCSP();         \
  POPN(n);

#define SPHANDLE(index)          \
  ({                             \
    SYNCSP();                    \
    Stack::handle(&sp[(index)]); \
  })
#define SPHANDLEMUT(index)          \
  ({                                \
    SYNCSP();                       \
    Stack::handleMut(&sp[(index)]); \
  })

template <bool IsRestart, bool HybridICs>
PBIResult PortableBaselineInterpret(
    JSContext* cx_, State& state, Stack& stack, StackVal* sp,
    JSObject* envChain, Value* ret, jsbytecode* pc, ImmutableScriptData* isd,
    jsbytecode* restartEntryPC, BaselineFrame* restartFrame,
    StackVal* restartEntryFrame, PBIResult restartCode) {
#define RESTART(code)                                                 \
  if (!IsRestart) {                                                   \
    TRACE_PRINTF("Restarting (code %d sp %p fp %p)\n", int(code), sp, \
                 ctx.stack.fp);                                       \
    SYNCSP();                                                         \
    restartCode = code;                                               \
    goto restart;                                                     \
  }

#define GOTO_ERROR()           \
  do {                         \
    SYNCSP();                  \
    RESTART(PBIResult::Error); \
    goto error;                \
  } while (0)

  // Update local state when we switch to a new script with a new PC.
#define RESET_PC(new_pc, new_script)                                \
  pc = new_pc;                                                      \
  entryPC = new_script->code();                                     \
  isd = new_script->immutableScriptData();                          \
  icEntries = frame->icScript()->icEntries();                       \
  icEntry = frame->interpreterICEntry();                            \
  argsObjAliasesFormals = frame->script()->argsObjAliasesFormals(); \
  resumeOffsets = isd->resumeOffsets().data();

#define OPCODE_LABEL(op, ...) LABEL(op),
#define TRAILING_LABEL(v) LABEL(default),

  static const void* const addresses[EnableInterruptsPseudoOpcode + 1] = {
      FOR_EACH_OPCODE(OPCODE_LABEL)
          FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_LABEL)};

#undef OPCODE_LABEL
#undef TRAILING_LABEL

  BaselineFrame* frame = restartFrame;
  StackVal* entryFrame = restartEntryFrame;
  jsbytecode* entryPC = restartEntryPC;

  if (!IsRestart) {
    PUSHNATIVE(StackValNative(nullptr));  // Fake return address.
    frame = stack.pushFrame(sp, cx_, envChain);
    MOZ_ASSERT(frame);  // safety: stack margin.
    sp = reinterpret_cast<StackVal*>(frame);
    // Save the entry frame so that when unwinding, we know when to
    // return from this C++ frame.
    entryFrame = sp;
    // Save the entry PC so that we can compute offsets locally.
    entryPC = pc;
  }

  bool from_unwind = false;
  uint32_t nfixed = frame->script()->nfixed();
  bool argsObjAliasesFormals = frame->script()->argsObjAliasesFormals();

  PBIResult ic_result = PBIResult::Ok;
  uint64_t ic_arg0 = 0, ic_arg1 = 0, ic_arg2 = 0, ic_ret = 0;

  ICCtx ctx(cx_, frame, state, stack);
  auto* icEntries = frame->icScript()->icEntries();
  auto* icEntry = icEntries;
  const uint32_t* resumeOffsets = isd->resumeOffsets().data();

  if (IsRestart) {
    ic_result = restartCode;
    TRACE_PRINTF(
        "Enter from restart: sp = %p ctx.stack.fp = %p ctx.frame = %p\n", sp,
        ctx.stack.fp, ctx.frame);
    goto ic_fail;
  } else {
    AutoCheckRecursionLimit recursion(ctx.frameMgr.cxForLocalUseOnly());
    if (!recursion.checkDontReport(ctx.frameMgr.cxForLocalUseOnly())) {
      PUSH_EXIT_FRAME();
      ReportOverRecursed(ctx.frameMgr.cxForLocalUseOnly());
      return PBIResult::Error;
    }
  }

  // Check max stack depth once, so we don't need to check it
  // otherwise below for ordinary stack-manipulation opcodes (just for
  // exit frames).
  if (!ctx.stack.check(sp, sizeof(StackVal) * frame->script()->nslots())) {
    PUSH_EXIT_FRAME();
    ReportOverRecursed(ctx.frameMgr.cxForLocalUseOnly());
    return PBIResult::Error;
  }

  SYNCSP();
  sp -= nfixed;
  for (uint32_t i = 0; i < nfixed; i++) {
    sp[i] = StackVal(UndefinedValue());
  }
  ret->setUndefined();

  // Check if we are being debugged, and set a flag in the frame if so. This
  // flag must be set before calling InitFunctionEnvironmentObjects.
  if (frame->script()->isDebuggee()) {
    TRACE_PRINTF("Script is debuggee\n");
    frame->setIsDebuggee();
  }

  if (CalleeTokenIsFunction(frame->calleeToken())) {
    JSFunction* func = CalleeTokenToFunction(frame->calleeToken());
    frame->setEnvironmentChain(func->environment());
    if (func->needsFunctionEnvironmentObjects()) {
      PUSH_EXIT_FRAME();
      if (!js::InitFunctionEnvironmentObjects(cx, frame)) {
        GOTO_ERROR();
      }
      TRACE_PRINTF("callee is func %p; created environment object: %p\n", func,
                   frame->environmentChain());
    }
  }

  // The debug prologue can't run until the function environment is set up.
  if (frame->script()->isDebuggee()) {
    PUSH_EXIT_FRAME();
    if (!DebugPrologue(cx, frame)) {
      GOTO_ERROR();
    }
  }

  if (!frame->script()->hasScriptCounts()) {
    if (ctx.frameMgr.cxForLocalUseOnly()->realm()->collectCoverageForDebug()) {
      PUSH_EXIT_FRAME();
      if (!frame->script()->initScriptCounts(cx)) {
        GOTO_ERROR();
      }
    }
  }
  COUNT_COVERAGE_MAIN();

#ifdef ENABLE_INTERRUPT_CHECKS
  if (ctx.frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
    PUSH_EXIT_FRAME();
    if (!InterruptCheck(cx)) {
      GOTO_ERROR();
    }
  }
#endif

  TRACE_PRINTF("Entering: sp = %p fp = %p frame = %p, script = %p, pc = %p\n",
               sp, ctx.stack.fp, frame, frame->script(), pc);
  TRACE_PRINTF("nslots = %d nfixed = %d\n", int(frame->script()->nslots()),
               int(frame->script()->nfixed()));

  while (true) {
    DEBUG_CHECK();

#if !defined(ENABLE_COMPUTED_GOTO_DISPATCH) || !defined(__wasi__)
  dispatch:
#endif

#ifdef TRACE_INTERP
  {
    JSOp op = JSOp(*pc);
    printf("sp[0] = %" PRIx64 " sp[1] = %" PRIx64 " sp[2] = %" PRIx64 "\n",
           sp[0].asUInt64(), sp[1].asUInt64(), sp[2].asUInt64());
    printf("script = %p pc = %p: %s (ic %d entry %p) pending = %d\n",
           frame->script(), pc, CodeName(op), (int)(icEntry - icEntries),
           icEntry, ctx.frameMgr.cxForLocalUseOnly()->isExceptionPending());
    printf("sp = %p fp = %p\n", sp, ctx.stack.fp);
    printf("TOS tag: %d\n", int(sp[0].asValue().asRawBits() >> 47));
    fflush(stdout);
  }
#endif

#ifdef ENABLE_COMPUTED_GOTO_DISPATCH
    goto* addresses[*pc];
#else
    (void)addresses;  // Avoid unused-local error. We keep the table
                      // itself to avoid warnings (see note in IC
                      // interpreter above).
    switch (JSOp(*pc))
#endif
    {
      CASE(Nop) { END_OP(Nop); }
      CASE(NopIsAssignOp) { END_OP(NopIsAssignOp); }
      CASE(Undefined) {
        VIRTPUSH(StackVal(UndefinedValue()));
        END_OP(Undefined);
      }
      CASE(Null) {
        VIRTPUSH(StackVal(NullValue()));
        END_OP(Null);
      }
      CASE(False) {
        VIRTPUSH(StackVal(BooleanValue(false)));
        END_OP(False);
      }
      CASE(True) {
        VIRTPUSH(StackVal(BooleanValue(true)));
        END_OP(True);
      }
      CASE(Int32) {
        VIRTPUSH(StackVal(Int32Value(GET_INT32(pc))));
        END_OP(Int32);
      }
      CASE(Zero) {
        VIRTPUSH(StackVal(Int32Value(0)));
        END_OP(Zero);
      }
      CASE(One) {
        VIRTPUSH(StackVal(Int32Value(1)));
        END_OP(One);
      }
      CASE(Int8) {
        VIRTPUSH(StackVal(Int32Value(GET_INT8(pc))));
        END_OP(Int8);
      }
      CASE(Uint16) {
        VIRTPUSH(StackVal(Int32Value(GET_UINT16(pc))));
        END_OP(Uint16);
      }
      CASE(Uint24) {
        VIRTPUSH(StackVal(Int32Value(GET_UINT24(pc))));
        END_OP(Uint24);
      }
      CASE(Double) {
        VIRTPUSH(StackVal(GET_INLINE_VALUE(pc)));
        END_OP(Double);
      }
      CASE(BigInt) {
        VIRTPUSH(StackVal(JS::BigIntValue(frame->script()->getBigInt(pc))));
        END_OP(BigInt);
      }
      CASE(String) {
        VIRTPUSH(StackVal(StringValue(frame->script()->getString(pc))));
        END_OP(String);
      }
      CASE(Symbol) {
        VIRTPUSH(StackVal(SymbolValue(
            ctx.frameMgr.cxForLocalUseOnly()->wellKnownSymbols().get(
                GET_UINT8(pc)))));
        END_OP(Symbol);
      }
      CASE(Void) {
        VIRTSPWRITE(0, StackVal(JS::UndefinedValue()));
        END_OP(Void);
      }

      CASE(Typeof)
      CASE(TypeofExpr) {
        static_assert(JSOpLength_Typeof == JSOpLength_TypeofExpr);
        if (HybridICs) {
          SYNCSP();
          VIRTSPWRITE(
              0,
              StackVal(StringValue(TypeOfOperation(
                  SPHANDLE(0), ctx.frameMgr.cxForLocalUseOnly()->runtime()))));
          NEXT_IC();
        } else {
          IC_POP_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC_AND_PUSH(TypeOf, false);
        }
        END_OP(Typeof);
      }

      CASE(TypeofEq) {
        if (HybridICs) {
          TypeofEqOperand operand =
              TypeofEqOperand::fromRawValue(GET_UINT8(pc));
          bool result = js::TypeOfValue(SPHANDLE(0)) == operand.type();
          if (operand.compareOp() == JSOp::Ne) {
            result = !result;
          }
          VIRTSPWRITE(0, StackVal(BooleanValue(result)));
          NEXT_IC();
        } else {
          IC_POP_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC_AND_PUSH(TypeOfEq, false);
        }
        END_OP(TypeofEq);
      }

      CASE(Pos) {
        if (VIRTSP(0).asValue().isNumber()) {
          // Nothing!
          NEXT_IC();
          END_OP(Pos);
        } else {
          goto generic_unary;
        }
      }
      CASE(Neg) {
        Value v = VIRTSP(0).asValue();
        if (v.isInt32()) {
          int32_t i = v.toInt32();
          if (i != 0 && i != INT32_MIN) {
            VIRTSPWRITE(0, StackVal(Int32Value(-i)));
            NEXT_IC();
            END_OP(Neg);
          }
        }
        if (v.isNumber()) {
          VIRTSPWRITE(0, StackVal(NumberValue(-v.toNumber())));
          NEXT_IC();
          END_OP(Neg);
        }
        goto generic_unary;
      }

      CASE(Inc) {
        Value v = VIRTSP(0).asValue();
        if (v.isInt32()) {
          int32_t i = v.toInt32();
          if (i != INT32_MAX) {
            VIRTSPWRITE(0, StackVal(Int32Value(i + 1)));
            NEXT_IC();
            END_OP(Inc);
          }
        }
        if (v.isNumber()) {
          VIRTSPWRITE(0, StackVal(NumberValue(v.toNumber() + 1)));
          NEXT_IC();
          END_OP(Inc);
        }
        goto generic_unary;
      }
      CASE(Dec) {
        Value v = VIRTSP(0).asValue();
        if (v.isInt32()) {
          int32_t i = v.toInt32();
          if (i != INT32_MIN) {
            VIRTSPWRITE(0, StackVal(Int32Value(i - 1)));
            NEXT_IC();
            END_OP(Dec);
          }
        }
        if (v.isNumber()) {
          VIRTSPWRITE(0, StackVal(NumberValue(v.toNumber() - 1)));
          NEXT_IC();
          END_OP(Dec);
        }
        goto generic_unary;
      }

      CASE(BitNot) {
        Value v = VIRTSP(0).asValue();
        if (v.isInt32()) {
          int32_t i = v.toInt32();
          VIRTSPWRITE(0, StackVal(Int32Value(~i)));
          NEXT_IC();
          END_OP(Inc);
        }
        goto generic_unary;
      }

      CASE(ToNumeric) {
        if (VIRTSP(0).asValue().isNumeric()) {
          NEXT_IC();
        } else if (HybridICs) {
          SYNCSP();
          MutableHandleValue val = SPHANDLEMUT(0);
          PUSH_EXIT_FRAME();
          if (!ToNumeric(cx, val)) {
            GOTO_ERROR();
          }
          NEXT_IC();
        } else {
          goto generic_unary;
        }
        END_OP(ToNumeric);
      }

    generic_unary:;
      {
        static_assert(JSOpLength_Pos == JSOpLength_Neg);
        static_assert(JSOpLength_Pos == JSOpLength_BitNot);
        static_assert(JSOpLength_Pos == JSOpLength_Inc);
        static_assert(JSOpLength_Pos == JSOpLength_Dec);
        static_assert(JSOpLength_Pos == JSOpLength_ToNumeric);
        IC_POP_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(UnaryArith, false);
        END_OP(Pos);
      }

      CASE(Not) {
        Value v = VIRTSP(0).asValue();
        if (v.isBoolean()) {
          VIRTSPWRITE(0, StackVal(BooleanValue(!v.toBoolean())));
          NEXT_IC();
        } else if (HybridICs) {
          SYNCSP();
          VIRTSPWRITE(0, StackVal(BooleanValue(!ToBoolean(SPHANDLE(0)))));
          NEXT_IC();
        } else {
          IC_POP_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC(ToBool, false);
          VIRTPUSH(
              StackVal(BooleanValue(!Value::fromRawBits(ic_ret).toBoolean())));
        }
        END_OP(Not);
      }

      CASE(And) {
        bool result;
        Value v = VIRTSP(0).asValue();
        if (v.isBoolean()) {
          result = v.toBoolean();
          NEXT_IC();
        } else if (HybridICs) {
          result = ToBoolean(SPHANDLE(0));
          NEXT_IC();
        } else {
          IC_SET_ARG_FROM_STACK(0, 0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC(ToBool, false);
          result = Value::fromRawBits(ic_ret).toBoolean();
        }
        int32_t jumpOffset = GET_JUMP_OFFSET(pc);
        if (!result) {
          ADVANCE(jumpOffset);
          PREDICT_NEXT(JumpTarget);
          PREDICT_NEXT(LoopHead);
        } else {
          ADVANCE(JSOpLength_And);
        }
        DISPATCH();
      }
      CASE(Or) {
        bool result;
        Value v = VIRTSP(0).asValue();
        if (v.isBoolean()) {
          result = v.toBoolean();
          NEXT_IC();
        } else if (HybridICs) {
          result = ToBoolean(SPHANDLE(0));
          NEXT_IC();
        } else {
          IC_SET_ARG_FROM_STACK(0, 0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC(ToBool, false);
          result = Value::fromRawBits(ic_ret).toBoolean();
        }
        int32_t jumpOffset = GET_JUMP_OFFSET(pc);
        if (result) {
          ADVANCE(jumpOffset);
          PREDICT_NEXT(JumpTarget);
          PREDICT_NEXT(LoopHead);
        } else {
          ADVANCE(JSOpLength_Or);
        }
        DISPATCH();
      }
      CASE(JumpIfTrue) {
        bool result;
        Value v = VIRTSP(0).asValue();
        if (v.isBoolean()) {
          result = v.toBoolean();
          VIRTPOP();
          NEXT_IC();
        } else if (HybridICs) {
          result = ToBoolean(SPHANDLE(0));
          VIRTPOP();
          NEXT_IC();
        } else {
          IC_POP_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC(ToBool, false);
          result = Value::fromRawBits(ic_ret).toBoolean();
        }
        int32_t jumpOffset = GET_JUMP_OFFSET(pc);
        if (result) {
          ADVANCE(jumpOffset);
          PREDICT_NEXT(JumpTarget);
          PREDICT_NEXT(LoopHead);
        } else {
          ADVANCE(JSOpLength_JumpIfTrue);
        }
        DISPATCH();
      }
      CASE(JumpIfFalse) {
        bool result;
        Value v = VIRTSP(0).asValue();
        if (v.isBoolean()) {
          result = v.toBoolean();
          VIRTPOP();
          NEXT_IC();
        } else if (HybridICs) {
          result = ToBoolean(SPHANDLE(0));
          VIRTPOP();
          NEXT_IC();
        } else {
          IC_POP_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC(ToBool, false);
          result = Value::fromRawBits(ic_ret).toBoolean();
        }
        int32_t jumpOffset = GET_JUMP_OFFSET(pc);
        if (!result) {
          ADVANCE(jumpOffset);
          PREDICT_NEXT(JumpTarget);
          PREDICT_NEXT(LoopHead);
        } else {
          ADVANCE(JSOpLength_JumpIfFalse);
        }
        DISPATCH();
      }

      CASE(Add) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int64_t lhs = v1.toInt32();
            int64_t rhs = v0.toInt32();
            int64_t result = lhs + rhs;
            if (result >= int64_t(INT32_MIN) && result <= int64_t(INT32_MAX)) {
              VIRTPOP();
              VIRTSPWRITE(0, StackVal(Int32Value(int32_t(result))));
              NEXT_IC();
              END_OP(Add);
            }
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(NumberValue(lhs + rhs)));
            NEXT_IC();
            END_OP(Add);
          }

          MutableHandleValue lhs = SPHANDLEMUT(1);
          MutableHandleValue rhs = SPHANDLEMUT(0);
          MutableHandleValue result = SPHANDLEMUT(1);
          {
            PUSH_EXIT_FRAME();
            if (!AddOperation(cx, lhs, rhs, result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          NEXT_IC();
          END_OP(Add);
        }
        goto generic_binary;
      }

      CASE(Sub) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int64_t lhs = v1.toInt32();
            int64_t rhs = v0.toInt32();
            int64_t result = lhs - rhs;
            if (result >= int64_t(INT32_MIN) && result <= int64_t(INT32_MAX)) {
              VIRTPOP();
              VIRTSPWRITE(0, StackVal(Int32Value(int32_t(result))));
              NEXT_IC();
              END_OP(Sub);
            }
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(NumberValue(lhs - rhs)));
            NEXT_IC();
            END_OP(Add);
          }

          MutableHandleValue lhs = SPHANDLEMUT(1);
          MutableHandleValue rhs = SPHANDLEMUT(0);
          MutableHandleValue result = SPHANDLEMUT(1);
          {
            PUSH_EXIT_FRAME();
            if (!SubOperation(cx, lhs, rhs, result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          NEXT_IC();
          END_OP(Sub);
        }
        goto generic_binary;
      }

      CASE(Mul) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int64_t lhs = v1.toInt32();
            int64_t rhs = v0.toInt32();
            int64_t product = lhs * rhs;
            if (product >= int64_t(INT32_MIN) &&
                product <= int64_t(INT32_MAX) &&
                (product != 0 || !((lhs < 0) ^ (rhs < 0)))) {
              VIRTPOP();
              VIRTSPWRITE(0, StackVal(Int32Value(int32_t(product))));
              NEXT_IC();
              END_OP(Mul);
            }
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(NumberValue(lhs * rhs)));
            NEXT_IC();
            END_OP(Mul);
          }

          MutableHandleValue lhs = SPHANDLEMUT(1);
          MutableHandleValue rhs = SPHANDLEMUT(0);
          MutableHandleValue result = SPHANDLEMUT(1);
          {
            PUSH_EXIT_FRAME();
            if (!MulOperation(cx, lhs, rhs, result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          NEXT_IC();
          END_OP(Mul);
        }
        goto generic_binary;
      }
      CASE(Div) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(NumberValue(NumberDiv(lhs, rhs))));
            NEXT_IC();
            END_OP(Div);
          }

          MutableHandleValue lhs = SPHANDLEMUT(1);
          MutableHandleValue rhs = SPHANDLEMUT(0);
          MutableHandleValue result = SPHANDLEMUT(1);
          {
            PUSH_EXIT_FRAME();
            if (!DivOperation(cx, lhs, rhs, result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          NEXT_IC();
          END_OP(Div);
        }
        goto generic_binary;
      }
      CASE(Mod) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int64_t lhs = v1.toInt32();
            int64_t rhs = v0.toInt32();
            if (lhs > 0 && rhs > 0) {
              int64_t mod = lhs % rhs;
              VIRTPOP();
              VIRTSPWRITE(0, StackVal(Int32Value(int32_t(mod))));
              NEXT_IC();
              END_OP(Mod);
            }
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(DoubleValue(NumberMod(lhs, rhs))));
            NEXT_IC();
            END_OP(Mod);
          }

          MutableHandleValue lhs = SPHANDLEMUT(1);
          MutableHandleValue rhs = SPHANDLEMUT(0);
          MutableHandleValue result = SPHANDLEMUT(1);
          {
            PUSH_EXIT_FRAME();
            if (!ModOperation(cx, lhs, rhs, result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          NEXT_IC();
          END_OP(Mod);
        }
        goto generic_binary;
      }
      CASE(Pow) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(NumberValue(ecmaPow(lhs, rhs))));
            NEXT_IC();
            END_OP(Pow);
          }

          MutableHandleValue lhs = SPHANDLEMUT(1);
          MutableHandleValue rhs = SPHANDLEMUT(0);
          MutableHandleValue result = SPHANDLEMUT(1);
          {
            PUSH_EXIT_FRAME();
            if (!PowOperation(cx, lhs, rhs, result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          NEXT_IC();
          END_OP(Pow);
        }
        goto generic_binary;
      }
      CASE(BitOr) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int32_t lhs = v1.toInt32();
            int32_t rhs = v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(Int32Value(lhs | rhs)));
            NEXT_IC();
            END_OP(BitOr);
          }
        }
        goto generic_binary;
      }
      CASE(BitAnd) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int32_t lhs = v1.toInt32();
            int32_t rhs = v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(Int32Value(lhs & rhs)));
            NEXT_IC();
            END_OP(BitAnd);
          }
        }
        goto generic_binary;
      }
      CASE(BitXor) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int32_t lhs = v1.toInt32();
            int32_t rhs = v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(Int32Value(lhs ^ rhs)));
            NEXT_IC();
            END_OP(BitXor);
          }
        }
        goto generic_binary;
      }
      CASE(Lsh) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            // Unsigned to avoid undefined behavior on left-shift overflow
            // (see comment in BitLshOperation in Interpreter.cpp).
            uint32_t lhs = uint32_t(v1.toInt32());
            uint32_t rhs = uint32_t(v0.toInt32());
            VIRTPOP();
            rhs &= 31;
            VIRTSPWRITE(0, StackVal(Int32Value(int32_t(lhs << rhs))));
            NEXT_IC();
            END_OP(Lsh);
          }
        }
        goto generic_binary;
      }
      CASE(Rsh) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            int32_t lhs = v1.toInt32();
            int32_t rhs = v0.toInt32();
            VIRTPOP();
            rhs &= 31;
            VIRTSPWRITE(0, StackVal(Int32Value(lhs >> rhs)));
            NEXT_IC();
            END_OP(Rsh);
          }
        }
        goto generic_binary;
      }
      CASE(Ursh) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            uint32_t lhs = uint32_t(v1.toInt32());
            int32_t rhs = v0.toInt32();
            VIRTPOP();
            rhs &= 31;
            uint32_t result = lhs >> rhs;
            StackVal stackResult(0);
            if (result <= uint32_t(INT32_MAX)) {
              stackResult = StackVal(Int32Value(int32_t(result)));
            } else {
              stackResult = StackVal(NumberValue(double(result)));
            }
            VIRTSPWRITE(0, stackResult);
            NEXT_IC();
            END_OP(Ursh);
          }
        }
        goto generic_binary;
      }

    generic_binary:;
      {
        static_assert(JSOpLength_BitOr == JSOpLength_BitXor);
        static_assert(JSOpLength_BitOr == JSOpLength_BitAnd);
        static_assert(JSOpLength_BitOr == JSOpLength_Lsh);
        static_assert(JSOpLength_BitOr == JSOpLength_Rsh);
        static_assert(JSOpLength_BitOr == JSOpLength_Ursh);
        static_assert(JSOpLength_BitOr == JSOpLength_Add);
        static_assert(JSOpLength_BitOr == JSOpLength_Sub);
        static_assert(JSOpLength_BitOr == JSOpLength_Mul);
        static_assert(JSOpLength_BitOr == JSOpLength_Div);
        static_assert(JSOpLength_BitOr == JSOpLength_Mod);
        static_assert(JSOpLength_BitOr == JSOpLength_Pow);
        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(BinaryArith, false);
        END_OP(Div);
      }

      CASE(Eq) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            bool result = v0.toInt32() == v1.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Eq);
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            bool result = lhs == rhs;
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Eq);
          }
          if (v0.isNumber() && v1.isNumber()) {
            bool result = v0.toNumber() == v1.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Eq);
          }
        }
        goto generic_cmp;
      }

      CASE(Ne) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            bool result = v0.toInt32() != v1.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Ne);
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            bool result = lhs != rhs;
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Ne);
          }
          if (v0.isNumber() && v1.isNumber()) {
            bool result = v0.toNumber() != v1.toNumber();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Eq);
          }
        }
        goto generic_cmp;
      }

      CASE(Lt) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            bool result = v1.toInt32() < v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Lt);
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            bool result = lhs < rhs;
            if (std::isnan(lhs) || std::isnan(rhs)) {
              result = false;
            }
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Lt);
          }
        }
        goto generic_cmp;
      }
      CASE(Le) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            bool result = v1.toInt32() <= v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Le);
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            bool result = lhs <= rhs;
            if (std::isnan(lhs) || std::isnan(rhs)) {
              result = false;
            }
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Le);
          }
        }
        goto generic_cmp;
      }
      CASE(Gt) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            bool result = v1.toInt32() > v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Gt);
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            bool result = lhs > rhs;
            if (std::isnan(lhs) || std::isnan(rhs)) {
              result = false;
            }
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Gt);
          }
        }
        goto generic_cmp;
      }
      CASE(Ge) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          if (v0.isInt32() && v1.isInt32()) {
            bool result = v1.toInt32() >= v0.toInt32();
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Ge);
          }
          if (v0.isNumber() && v1.isNumber()) {
            double lhs = v1.toNumber();
            double rhs = v0.toNumber();
            bool result = lhs >= rhs;
            if (std::isnan(lhs) || std::isnan(rhs)) {
              result = false;
            }
            VIRTPOP();
            VIRTSPWRITE(0, StackVal(BooleanValue(result)));
            NEXT_IC();
            END_OP(Ge);
          }
        }
        goto generic_cmp;
      }

      CASE(StrictEq)
      CASE(StrictNe) {
        if (HybridICs) {
          Value v0 = VIRTSP(0).asValue();
          Value v1 = VIRTSP(1).asValue();
          bool result;
          HandleValue lval = SPHANDLE(1);
          HandleValue rval = SPHANDLE(0);
          if (v0.isString() && v1.isString()) {
            PUSH_EXIT_FRAME();
            if (!js::StrictlyEqual(cx, lval, rval, &result)) {
              GOTO_ERROR();
            }
          } else {
            if (!js::StrictlyEqual(nullptr, lval, rval, &result)) {
              GOTO_ERROR();
            }
          }
          VIRTPOP();
          VIRTSPWRITE(0,
                      StackVal(BooleanValue(
                          (JSOp(*pc) == JSOp::StrictEq) ? result : !result)));
          NEXT_IC();
          END_OP(StrictEq);
        } else {
          goto generic_cmp;
        }
      }

    generic_cmp:;
      {
        static_assert(JSOpLength_Eq == JSOpLength_Ne);
        static_assert(JSOpLength_Eq == JSOpLength_StrictEq);
        static_assert(JSOpLength_Eq == JSOpLength_StrictNe);
        static_assert(JSOpLength_Eq == JSOpLength_Lt);
        static_assert(JSOpLength_Eq == JSOpLength_Gt);
        static_assert(JSOpLength_Eq == JSOpLength_Le);
        static_assert(JSOpLength_Eq == JSOpLength_Ge);
        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(Compare, false);
        END_OP(Eq);
      }

      CASE(StrictConstantNe)
      CASE(StrictConstantEq) {
        JSOp op = JSOp(*pc);
        uint16_t operand = GET_UINT16(pc);
        {
          ReservedRooted<JS::Value> val(&state.value0, VIRTPOP().asValue());
          bool result;
          {
            PUSH_EXIT_FRAME();
            if (!js::ConstantStrictEqual(cx, val, operand, &result)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(
              BooleanValue(op == JSOp::StrictConstantEq ? result : !result)));
        }
        END_OP(StrictConstantEq);
      }

      CASE(Instanceof) {
        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(InstanceOf, false);
        END_OP(Instanceof);
      }

      CASE(In) {
        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(In, false);
        END_OP(In);
      }

      CASE(ToPropertyKey) {
        IC_POP_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(ToPropertyKey, false);
        END_OP(ToPropertyKey);
      }

      CASE(ToString) {
        if (VIRTSP(0).asValue().isString()) {
          END_OP(ToString);
        }
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          if (JSString* result = ToStringSlow<NoGC>(
                  ctx.frameMgr.cxForLocalUseOnly(), value0)) {
            VIRTPUSH(StackVal(StringValue(result)));
          } else {
            {
              PUSH_EXIT_FRAME();
              result = ToString<CanGC>(cx, value0);
              if (!result) {
                GOTO_ERROR();
              }
            }
            VIRTPUSH(StackVal(StringValue(result)));
          }
        }
        END_OP(ToString);
      }

      CASE(IsNullOrUndefined) {
        Value v = VIRTSP(0).asValue();
        bool result = v.isNull() || v.isUndefined();
        VIRTPUSH(StackVal(BooleanValue(result)));
        END_OP(IsNullOrUndefined);
      }

      CASE(GlobalThis) {
        VIRTPUSH(StackVal(ObjectValue(*ctx.frameMgr.cxForLocalUseOnly()
                                           ->global()
                                           ->lexicalEnvironment()
                                           .thisObject())));
        END_OP(GlobalThis);
      }

      CASE(NonSyntacticGlobalThis) {
        {
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         frame->environmentChain());
          ReservedRooted<Value> value0(&state.value0);
          {
            PUSH_EXIT_FRAME();
            js::GetNonSyntacticGlobalThis(cx, obj0, &value0);
          }
          VIRTPUSH(StackVal(value0));
        }
        END_OP(NonSyntacticGlobalThis);
      }

      CASE(NewTarget) {
        VIRTPUSH(StackVal(frame->newTarget()));
        END_OP(NewTarget);
      }

      CASE(DynamicImport) {
        {
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // options
          ReservedRooted<Value> value1(&state.value1,
                                       VIRTPOP().asValue());  // specifier
          JSObject* promise;
          {
            PUSH_EXIT_FRAME();
            ReservedRooted<JSScript*> script0(&state.script0, frame->script());
            promise = StartDynamicModuleImport(cx, script0, value1, value0);
            if (!promise) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*promise)));
        }
        END_OP(DynamicImport);
      }

      CASE(ImportMeta) {
        IC_ZERO_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(LazyConstant, false);
        END_OP(ImportMeta);
      }

      CASE(NewInit) {
        if (HybridICs) {
          JSObject* obj;
          {
            PUSH_EXIT_FRAME();
            ReservedRooted<JSScript*> script0(&state.script0, frame->script());
            obj = NewObjectOperation(cx, script0, pc);
            if (!obj) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*obj)));
          NEXT_IC();
          END_OP(NewInit);
        } else {
          IC_ZERO_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC_AND_PUSH(NewObject, false);
          END_OP(NewInit);
        }
      }
      CASE(NewObject) {
        if (HybridICs) {
          JSObject* obj;
          {
            PUSH_EXIT_FRAME();
            ReservedRooted<JSScript*> script0(&state.script0, frame->script());
            obj = NewObjectOperation(cx, script0, pc);
            if (!obj) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*obj)));
          NEXT_IC();
          END_OP(NewObject);
        } else {
          IC_ZERO_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC_AND_PUSH(NewObject, false);
          END_OP(NewObject);
        }
      }
      CASE(Object) {
        VIRTPUSH(StackVal(ObjectValue(*frame->script()->getObject(pc))));
        END_OP(Object);
      }
      CASE(ObjWithProto) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTSP(0).asValue());
          JSObject* obj;
          {
            PUSH_EXIT_FRAME();
            obj = ObjectWithProtoOperation(cx, value0);
            if (!obj) {
              GOTO_ERROR();
            }
          }
          VIRTSPWRITE(0, StackVal(ObjectValue(*obj)));
        }
        END_OP(ObjWithProto);
      }

      CASE(InitElem)
      CASE(InitHiddenElem)
      CASE(InitLockedElem)
      CASE(InitElemInc)
      CASE(SetElem)
      CASE(StrictSetElem) {
        static_assert(JSOpLength_InitElem == JSOpLength_InitHiddenElem);
        static_assert(JSOpLength_InitElem == JSOpLength_InitLockedElem);
        static_assert(JSOpLength_InitElem == JSOpLength_InitElemInc);
        static_assert(JSOpLength_InitElem == JSOpLength_SetElem);
        static_assert(JSOpLength_InitElem == JSOpLength_StrictSetElem);
        StackVal val = VIRTSP(0);
        IC_POP_ARG(2);
        IC_POP_ARG(1);
        IC_SET_ARG_FROM_STACK(0, 0);
        if (JSOp(*pc) == JSOp::SetElem || JSOp(*pc) == JSOp::StrictSetElem) {
          VIRTSPWRITE(0, val);
        }
        INVOKE_IC(SetElem, true);
        if (JSOp(*pc) == JSOp::InitElemInc) {
          VIRTPUSH(
              StackVal(Int32Value(Value::fromRawBits(ic_arg1).toInt32() + 1)));
        }
        END_OP(InitElem);
      }

      CASE(InitPropGetter)
      CASE(InitHiddenPropGetter)
      CASE(InitPropSetter)
      CASE(InitHiddenPropSetter) {
        static_assert(JSOpLength_InitPropGetter ==
                      JSOpLength_InitHiddenPropGetter);
        static_assert(JSOpLength_InitPropGetter == JSOpLength_InitPropSetter);
        static_assert(JSOpLength_InitPropGetter ==
                      JSOpLength_InitHiddenPropSetter);
        {
          ReservedRooted<JSObject*> obj1(
              &state.obj1,
              &VIRTPOP().asValue().toObject());  // val
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTSP(0).asValue().toObject());  // obj; leave on stack
          ReservedRooted<PropertyName*> name0(&state.name0,
                                              frame->script()->getName(pc));
          {
            PUSH_EXIT_FRAME();
            if (!InitPropGetterSetterOperation(cx, pc, obj0, name0, obj1)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(InitPropGetter);
      }

      CASE(InitElemGetter)
      CASE(InitHiddenElemGetter)
      CASE(InitElemSetter)
      CASE(InitHiddenElemSetter) {
        static_assert(JSOpLength_InitElemGetter ==
                      JSOpLength_InitHiddenElemGetter);
        static_assert(JSOpLength_InitElemGetter == JSOpLength_InitElemSetter);
        static_assert(JSOpLength_InitElemGetter ==
                      JSOpLength_InitHiddenElemSetter);
        {
          ReservedRooted<JSObject*> obj1(
              &state.obj1,
              &VIRTPOP().asValue().toObject());  // val
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // idval
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTSP(0).asValue().toObject());  // obj; leave on stack
          {
            PUSH_EXIT_FRAME();
            if (!InitElemGetterSetterOperation(cx, pc, obj0, value0, obj1)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(InitElemGetter);
      }

      CASE(GetProp)
      CASE(GetBoundName) {
        static_assert(JSOpLength_GetProp == JSOpLength_GetBoundName);
        IC_POP_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetProp, false);
        END_OP(GetProp);
      }
      CASE(GetPropSuper) {
        IC_POP_ARG(0);
        IC_POP_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetPropSuper, false);
        END_OP(GetPropSuper);
      }

      CASE(GetElem) {
        if (HybridICs && VIRTSP(1).asValue().isString()) {
          HandleValue lhs = SPHANDLE(1);
          HandleValue rhs = SPHANDLE(0);
          uint32_t index;
          if (IsDefinitelyIndex(rhs, &index)) {
            JSString* str = lhs.toString();
            if (index < str->length() && str->isLinear()) {
              JSLinearString* linear = &str->asLinear();
              char16_t c = linear->latin1OrTwoByteChar(index);
              StaticStrings& sstr =
                  ctx.frameMgr.cxForLocalUseOnly()->staticStrings();
              if (sstr.hasUnit(c)) {
                VIRTPOP();
                VIRTSPWRITE(0, StackVal(StringValue(sstr.getUnit(c))));
                NEXT_IC();
                END_OP(GetElem);
              }
            }
          }
        }

        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetElem, false);
        END_OP(GetElem);
      }

      CASE(GetElemSuper) {
        // N.B.: second and third args are out of order! See the saga at
        // https://bugzilla.mozilla.org/show_bug.cgi?id=1709328; this is
        // an echo of that issue.
        IC_POP_ARG(1);
        IC_POP_ARG(2);
        IC_POP_ARG(0);
        INVOKE_IC_AND_PUSH(GetElemSuper, true);
        END_OP(GetElemSuper);
      }

      CASE(DelProp) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          ReservedRooted<PropertyName*> name0(&state.name0,
                                              frame->script()->getName(pc));
          bool res = false;
          {
            PUSH_EXIT_FRAME();
            if (!DelPropOperation<false>(cx, value0, name0, &res)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(BooleanValue(res)));
        }
        END_OP(DelProp);
      }
      CASE(StrictDelProp) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          ReservedRooted<PropertyName*> name0(&state.name0,
                                              frame->script()->getName(pc));
          bool res = false;
          {
            PUSH_EXIT_FRAME();
            if (!DelPropOperation<true>(cx, value0, name0, &res)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(BooleanValue(res)));
        }
        END_OP(StrictDelProp);
      }
      CASE(DelElem) {
        {
          ReservedRooted<Value> value1(&state.value1, VIRTPOP().asValue());
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          bool res = false;
          {
            PUSH_EXIT_FRAME();
            if (!DelElemOperation<false>(cx, value0, value1, &res)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(BooleanValue(res)));
        }
        END_OP(DelElem);
      }
      CASE(StrictDelElem) {
        {
          ReservedRooted<Value> value1(&state.value1, VIRTPOP().asValue());
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          bool res = false;
          {
            PUSH_EXIT_FRAME();
            if (!DelElemOperation<true>(cx, value0, value1, &res)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(BooleanValue(res)));
        }
        END_OP(StrictDelElem);
      }

      CASE(HasOwn) {
        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(HasOwn, false);
        END_OP(HasOwn);
      }

      CASE(CheckPrivateField) {
        IC_SET_ARG_FROM_STACK(1, 0);
        IC_SET_ARG_FROM_STACK(0, 1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(CheckPrivateField, false);
        END_OP(CheckPrivateField);
      }

      CASE(NewPrivateName) {
        {
          ReservedRooted<JSAtom*> atom0(&state.atom0,
                                        frame->script()->getAtom(pc));
          JS::Symbol* symbol;
          {
            PUSH_EXIT_FRAME();
            symbol = NewPrivateName(cx, atom0);
            if (!symbol) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(SymbolValue(symbol)));
        }
        END_OP(NewPrivateName);
      }

      CASE(SuperBase) {
        JSFunction& superEnvFunc =
            VIRTPOP().asValue().toObject().as<JSFunction>();
        MOZ_ASSERT(superEnvFunc.allowSuperProperty());
        MOZ_ASSERT(superEnvFunc.baseScript()->needsHomeObject());
        const Value& homeObjVal = superEnvFunc.getExtendedSlot(
            FunctionExtended::METHOD_HOMEOBJECT_SLOT);

        JSObject* homeObj = &homeObjVal.toObject();
        JSObject* superBase = HomeObjectSuperBase(homeObj);

        VIRTPUSH(StackVal(ObjectOrNullValue(superBase)));
        END_OP(SuperBase);
      }

      CASE(SetPropSuper)
      CASE(StrictSetPropSuper) {
        // stack signature: receiver, lval, rval => rval
        static_assert(JSOpLength_SetPropSuper == JSOpLength_StrictSetPropSuper);
        bool strict = JSOp(*pc) == JSOp::StrictSetPropSuper;
        {
          ReservedRooted<Value> value2(&state.value2,
                                       VIRTPOP().asValue());  // rval
          ReservedRooted<Value> value1(&state.value1,
                                       VIRTPOP().asValue());  // lval
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // recevier
          ReservedRooted<PropertyName*> name0(&state.name0,
                                              frame->script()->getName(pc));
          {
            PUSH_EXIT_FRAME();
            // SetPropertySuper(cx, lval, receiver, name, rval, strict)
            // (N.B.: lval and receiver are transposed!)
            if (!SetPropertySuper(cx, value1, value0, name0, value2, strict)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(value2));
        }
        END_OP(SetPropSuper);
      }

      CASE(SetElemSuper)
      CASE(StrictSetElemSuper) {
        // stack signature: receiver, key, lval, rval => rval
        static_assert(JSOpLength_SetElemSuper == JSOpLength_StrictSetElemSuper);
        bool strict = JSOp(*pc) == JSOp::StrictSetElemSuper;
        {
          ReservedRooted<Value> value3(&state.value3,
                                       VIRTPOP().asValue());  // rval
          ReservedRooted<Value> value2(&state.value2,
                                       VIRTPOP().asValue());  // lval
          ReservedRooted<Value> value1(&state.value1,
                                       VIRTPOP().asValue());  // index
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // receiver
          {
            PUSH_EXIT_FRAME();
            // SetElementSuper(cx, lval, receiver, index, rval, strict)
            // (N.B.: lval, receiver and index are rotated!)
            if (!SetElementSuper(cx, value2, value0, value1, value3, strict)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(value3));  // value
        }
        END_OP(SetElemSuper);
      }

      CASE(Iter) {
        IC_POP_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetIterator, false);
        END_OP(Iter);
      }

      CASE(MoreIter) {
        // iter => iter, name
        Value v = IteratorMore(&VIRTSP(0).asValue().toObject());
        VIRTPUSH(StackVal(v));
        END_OP(MoreIter);
      }

      CASE(IsNoIter) {
        // iter => iter, bool
        bool result = VIRTSP(0).asValue().isMagic(JS_NO_ITER_VALUE);
        VIRTPUSH(StackVal(BooleanValue(result)));
        END_OP(IsNoIter);
      }

      CASE(EndIter) {
        // iter, interval =>
        VIRTPOP();
        CloseIterator(&VIRTPOP().asValue().toObject());
        END_OP(EndIter);
      }

      CASE(CloseIter) {
        IC_SET_OBJ_ARG(0, &VIRTPOP().asValue().toObject());
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC(CloseIter, false);
        END_OP(CloseIter);
      }

      CASE(CheckIsObj) {
        if (!VIRTSP(0).asValue().isObject()) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(
              js::ThrowCheckIsObject(cx, js::CheckIsObjectKind(GET_UINT8(pc))));
          /* abandon frame; error handler will re-establish sp */
          GOTO_ERROR();
        }
        END_OP(CheckIsObj);
      }

      CASE(CheckObjCoercible) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTSP(0).asValue());
          if (value0.isNullOrUndefined()) {
            PUSH_EXIT_FRAME();
            MOZ_ALWAYS_FALSE(ThrowObjectCoercible(cx, value0));
            /* abandon frame; error handler will re-establish sp */
            GOTO_ERROR();
          }
        }
        END_OP(CheckObjCoercible);
      }

      CASE(ToAsyncIter) {
        // iter, next => asynciter
        {
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // next
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTPOP().asValue().toObject());  // iter
          JSObject* result;
          {
            PUSH_EXIT_FRAME();
            result = CreateAsyncFromSyncIterator(cx, obj0, value0);
            if (!result) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*result)));
        }
        END_OP(ToAsyncIter);
      }

      CASE(MutateProto) {
        // obj, protoVal => obj
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         &VIRTSP(0).asValue().toObject());
          {
            PUSH_EXIT_FRAME();
            if (!MutatePrototype(cx, obj0.as<PlainObject>(), value0)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(MutateProto);
      }

      CASE(NewArray) {
        if (HybridICs) {
          ArrayObject* obj;
          {
            PUSH_EXIT_FRAME();
            uint32_t length = GET_UINT32(pc);
            obj = NewArrayOperation(cx, length);
            if (!obj) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*obj)));
          NEXT_IC();
          END_OP(NewArray);
        } else {
          IC_ZERO_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC_AND_PUSH(NewArray, false);
          END_OP(NewArray);
        }
      }

      CASE(InitElemArray) {
        // array, val => array
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         &VIRTSP(0).asValue().toObject());
          {
            PUSH_EXIT_FRAME();
            InitElemArrayOperation(cx, pc, obj0.as<ArrayObject>(), value0);
          }
        }
        END_OP(InitElemArray);
      }

      CASE(Hole) {
        VIRTPUSH(StackVal(MagicValue(JS_ELEMENTS_HOLE)));
        END_OP(Hole);
      }

      CASE(RegExp) {
        JSObject* obj;
        {
          PUSH_EXIT_FRAME();
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         frame->script()->getRegExp(pc));
          obj = CloneRegExpObject(cx, obj0.as<RegExpObject>());
          if (!obj) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(ObjectValue(*obj)));
        END_OP(RegExp);
      }

      CASE(Lambda) {
        if (HybridICs) {
          JSObject* clone;
          {
            PUSH_EXIT_FRAME();
            ReservedRooted<JSFunction*> fun0(&state.fun0,
                                             frame->script()->getFunction(pc));
            ReservedRooted<JSObject*> obj0(&state.obj0,
                                           frame->environmentChain());
            clone = Lambda(cx, fun0, obj0);
            if (!clone) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*clone)));
          NEXT_IC();
          END_OP(Lambda);
        } else {
          IC_ZERO_ARG(0);
          IC_ZERO_ARG(1);
          IC_ZERO_ARG(2);
          INVOKE_IC_AND_PUSH(Lambda, false);
          END_OP(Lambda);
        }
      }

      CASE(SetFunName) {
        // fun, name => fun
        {
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // name
          ReservedRooted<JSFunction*> fun0(
              &state.fun0, &VIRTSP(0).asValue().toObject().as<JSFunction>());
          FunctionPrefixKind prefixKind = FunctionPrefixKind(GET_UINT8(pc));
          {
            PUSH_EXIT_FRAME();
            if (!SetFunctionName(cx, fun0, value0, prefixKind)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(SetFunName);
      }

      CASE(InitHomeObject) {
        // fun, homeObject => fun
        {
          ReservedRooted<JSObject*> obj0(
              &state.obj0, &VIRTPOP().asValue().toObject());  // homeObject
          ReservedRooted<JSFunction*> fun0(
              &state.fun0, &VIRTSP(0).asValue().toObject().as<JSFunction>());
          MOZ_ASSERT(fun0->allowSuperProperty());
          MOZ_ASSERT(obj0->is<PlainObject>() || obj0->is<JSFunction>());
          fun0->setExtendedSlot(FunctionExtended::METHOD_HOMEOBJECT_SLOT,
                                ObjectValue(*obj0));
        }
        END_OP(InitHomeObject);
      }

      CASE(CheckClassHeritage) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTSP(0).asValue());
          {
            PUSH_EXIT_FRAME();
            if (!CheckClassHeritageOperation(cx, value0)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(CheckClassHeritage);
      }

      CASE(FunWithProto) {
        // proto => obj
        {
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTPOP().asValue().toObject());  // proto
          ReservedRooted<JSObject*> obj1(&state.obj1,
                                         frame->environmentChain());
          ReservedRooted<JSFunction*> fun0(&state.fun0,
                                           frame->script()->getFunction(pc));
          JSObject* obj;
          {
            PUSH_EXIT_FRAME();
            obj = FunWithProtoOperation(cx, fun0, obj1, obj0);
            if (!obj) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(ObjectValue(*obj)));
        }
        END_OP(FunWithProto);
      }

      CASE(BuiltinObject) {
        IC_ZERO_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(LazyConstant, false);
        END_OP(BuiltinObject);
      }

      CASE(Call)
      CASE(CallIgnoresRv)
      CASE(CallContent)
      CASE(CallIter)
      CASE(CallContentIter)
      CASE(Eval)
      CASE(StrictEval)
      CASE(SuperCall)
      CASE(New)
      CASE(NewContent) {
        static_assert(JSOpLength_Call == JSOpLength_CallIgnoresRv);
        static_assert(JSOpLength_Call == JSOpLength_CallContent);
        static_assert(JSOpLength_Call == JSOpLength_CallIter);
        static_assert(JSOpLength_Call == JSOpLength_CallContentIter);
        static_assert(JSOpLength_Call == JSOpLength_Eval);
        static_assert(JSOpLength_Call == JSOpLength_StrictEval);
        static_assert(JSOpLength_Call == JSOpLength_SuperCall);
        static_assert(JSOpLength_Call == JSOpLength_New);
        static_assert(JSOpLength_Call == JSOpLength_NewContent);
        JSOp op = JSOp(*pc);
        bool constructing = (op == JSOp::New || op == JSOp::NewContent ||
                             op == JSOp::SuperCall);
        uint32_t argc = GET_ARGC(pc);
        do {
          {
            // CallArgsFromSp would be called with
            // - numValues = argc + 2 + constructing
            // - stackSlots = argc + constructing
            // - sp = vp + numValues
            // CallArgs::create then gets
            // - argc_ = stackSlots - constructing = argc
            // - argv_ = sp - stackSlots = vp + 2
            // our arguments are in reverse order compared to what CallArgs
            // expects so we should subtract any array subscripts from (sp +
            // stackSlots - 1)
            StackVal* firstArg = sp + argc + constructing - 1;

            // callee is argv_[-2] -> sp + argc + constructing + 1
            // this is   argv_[-1] -> sp + argc + constructing
            // newTarget is argv_[argc_] -> sp + constructing - 1
            // but this/newTarget are only used when constructing is 1 so we can
            // simplify this is   argv_[-1] -> sp + argc + 1 newTarget is
            // argv_[argc_] -> sp

            HandleValue callee = Stack::handle(firstArg + 2);
            if (!callee.isObject() || !callee.toObject().is<JSFunction>()) {
              TRACE_PRINTF("missed fastpath: not a function\n");
              break;
            }
            ReservedRooted<JSFunction*> func(
                &state.fun0, &callee.toObject().as<JSFunction>());
            if (!func->hasBaseScript() || !func->isInterpreted()) {
              TRACE_PRINTF("missed fastpath: not an interpreted script\n");
              break;
            }
            if (!constructing && func->isClassConstructor()) {
              TRACE_PRINTF(
                  "missed fastpath: constructor called without `new`\n");
              break;
            }
            if (constructing && !func->isConstructor()) {
              TRACE_PRINTF(
                  "missed fastpath: constructing with a non-constructor\n");
              break;
            }
            if (!func->baseScript()->hasBytecode()) {
              TRACE_PRINTF("missed fastpath: no bytecode\n");
              break;
            }
            ReservedRooted<JSScript*> calleeScript(
                &state.script0, func->baseScript()->asJSScript());
            if (!calleeScript->hasJitScript()) {
              TRACE_PRINTF("missed fastpath: no jit-script\n");
              break;
            }
            if (ctx.frameMgr.cxForLocalUseOnly()->realm() !=
                calleeScript->realm()) {
              TRACE_PRINTF("missed fastpath: mismatched realm\n");
              break;
            }
            if (argc < func->nargs()) {
              TRACE_PRINTF("missed fastpath: not enough arguments\n");
              break;
            }

            // Fast-path: function, interpreted, has JitScript, same realm, no
            // argument underflow.

            // Include newTarget in the args if it exists; exclude callee
            uint32_t totalArgs = argc + 1 + constructing;
            StackVal* origArgs = sp;

            TRACE_PRINTF(
                "Call fastpath: argc = %d origArgs = %p callee = %" PRIx64 "\n",
                argc, origArgs, callee.get().asRawBits());

            if (!ctx.stack.check(sp, sizeof(StackVal) * (totalArgs + 3))) {
              TRACE_PRINTF("missed fastpath: would cause stack overrun\n");
              break;
            }

            if (constructing) {
              MutableHandleValue thisv = Stack::handleMut(firstArg + 1);
              if (!thisv.isObject()) {
                HandleValue newTarget = Stack::handle(firstArg - argc);
                ReservedRooted<JSObject*> obj0(&state.obj0,
                                               &newTarget.toObject());

                PUSH_EXIT_FRAME();
                // CreateThis might discard the JitScript but we're counting on
                // it continuing to exist while we evaluate the fastpath.
                AutoKeepJitScripts keepJitScript(cx);
                if (!CreateThis(cx, func, obj0, GenericObject, thisv)) {
                  GOTO_ERROR();
                }

                TRACE_PRINTF("created %" PRIx64 "\n", thisv.get().asRawBits());
              }
            }

            // 0. Save current PC and interpreter IC pointer in
            // current frame, so we can retrieve them later.
            frame->interpreterPC() = pc;
            frame->interpreterICEntry() = icEntry;

            // 1. Push a baseline stub frame. Don't use the frame manager
            // -- we don't want the frame to be auto-freed when we leave
            // this scope, and we don't want to shadow `sp`.
            StackVal* exitFP = ctx.stack.pushExitFrame(sp, frame);
            MOZ_ASSERT(exitFP);  // safety: stack margin.
            sp = exitFP;
            TRACE_PRINTF("exit frame at %p\n", exitFP);

            // 2. Modify exit code to nullptr (this is where ICStubReg is
            // normally saved; the tracing code can skip if null).
            PUSHNATIVE(StackValNative(nullptr));

            // 3. Push args in proper order (they are reversed in our
            // downward-growth stack compared to what the calling
            // convention expects).
            for (uint32_t i = 0; i < totalArgs; i++) {
              VIRTPUSH(origArgs[i]);
            }

            // 4. Push inter-frame content: callee token, descriptor for
            // above.
            PUSHNATIVE(StackValNative(CalleeToToken(func, constructing)));
            PUSHNATIVE(StackValNative(
                MakeFrameDescriptorForJitCall(FrameType::BaselineStub, argc)));

            // 5. Push fake return address, set script, push baseline frame.
            PUSHNATIVE(StackValNative(nullptr));
            BaselineFrame* newFrame =
                ctx.stack.pushFrame(sp, ctx.frameMgr.cxForLocalUseOnly(),
                                    /* envChain = */ func->environment());
            MOZ_ASSERT(newFrame);  // safety: stack margin.
            TRACE_PRINTF("callee frame at %p\n", newFrame);
            frame = newFrame;
            ctx.frameMgr.switchToFrame(frame);
            ctx.frame = frame;
            // 6. Set up PC and SP for callee.
            sp = reinterpret_cast<StackVal*>(frame);
            RESET_PC(calleeScript->code(), calleeScript);
            // 7. Check callee stack space for max stack depth.
            if (!stack.check(sp, sizeof(StackVal) * calleeScript->nslots())) {
              PUSH_EXIT_FRAME();
              ReportOverRecursed(ctx.frameMgr.cxForLocalUseOnly());
              GOTO_ERROR();
            }
            // 8. Push local slots, and set return value to `undefined` by
            // default.
            uint32_t nfixed = calleeScript->nfixed();
            for (uint32_t i = 0; i < nfixed; i++) {
              VIRTPUSH(StackVal(UndefinedValue()));
            }
            ret->setUndefined();
            // 9. Initialize environment objects.
            if (func->needsFunctionEnvironmentObjects()) {
              PUSH_EXIT_FRAME();
              if (!js::InitFunctionEnvironmentObjects(cx, frame)) {
                GOTO_ERROR();
              }
            }
            // 10. Set debug flag, if appropriate.
            if (frame->script()->isDebuggee()) {
              TRACE_PRINTF("Script is debuggee\n");
              frame->setIsDebuggee();

              PUSH_EXIT_FRAME();
              if (!DebugPrologue(cx, frame)) {
                GOTO_ERROR();
              }
            }
            // 11. Check for interrupts.
#ifdef ENABLE_INTERRUPT_CHECKS
            if (ctx.frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
              PUSH_EXIT_FRAME();
              if (!InterruptCheck(cx)) {
                GOTO_ERROR();
              }
            }
#endif
            // 12. Initialize coverage tables, if needed.
            if (!frame->script()->hasScriptCounts()) {
              if (ctx.frameMgr.cxForLocalUseOnly()
                      ->realm()
                      ->collectCoverageForDebug()) {
                PUSH_EXIT_FRAME();
                if (!frame->script()->initScriptCounts(cx)) {
                  GOTO_ERROR();
                }
              }
            }
            COUNT_COVERAGE_MAIN();
          }

          // Everything is switched to callee context now -- dispatch!
          DISPATCH();
        } while (0);

        // Slow path: use the IC!
        ic_arg0 = argc;
        ctx.icregs.extraArgs = 2 + constructing;
        INVOKE_IC(Call, false);
        VIRTPOPN(argc + 2 + constructing);
        VIRTPUSH(StackVal(Value::fromRawBits(ic_ret)));
        END_OP(Call);
      }

      CASE(SpreadCall)
      CASE(SpreadEval)
      CASE(StrictSpreadEval) {
        static_assert(JSOpLength_SpreadCall == JSOpLength_SpreadEval);
        static_assert(JSOpLength_SpreadCall == JSOpLength_StrictSpreadEval);
        ic_arg0 = 1;
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        ctx.icregs.extraArgs = 2;
        INVOKE_IC(SpreadCall, false);
        VIRTPOPN(3);
        VIRTPUSH(StackVal(Value::fromRawBits(ic_ret)));
        END_OP(SpreadCall);
      }

      CASE(SpreadSuperCall)
      CASE(SpreadNew) {
        static_assert(JSOpLength_SpreadSuperCall == JSOpLength_SpreadNew);
        ic_arg0 = 1;
        ctx.icregs.extraArgs = 3;
        INVOKE_IC(SpreadCall, false);
        VIRTPOPN(4);
        VIRTPUSH(StackVal(Value::fromRawBits(ic_ret)));
        END_OP(SpreadSuperCall);
      }

      CASE(OptimizeSpreadCall) {
        IC_POP_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(OptimizeSpreadCall, false);
        END_OP(OptimizeSpreadCall);
      }

      CASE(OptimizeGetIterator) {
        IC_POP_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(OptimizeGetIterator, false);
        END_OP(OptimizeGetIterator);
      }

      CASE(ImplicitThis) {
        {
          ReservedRooted<JSObject*> env(&state.obj0,
                                        &VIRTSP(0).asValue().toObject());
          VIRTPOP();
          PUSH_EXIT_FRAME();
          ImplicitThisOperation(cx, env, &state.res);
        }
        VIRTPUSH(StackVal(state.res));
        state.res.setUndefined();
        END_OP(ImplicitThis);
      }

      CASE(CallSiteObj) {
        JSObject* cso = frame->script()->getObject(pc);
        MOZ_ASSERT(!cso->as<ArrayObject>().isExtensible());
        MOZ_ASSERT(cso->as<ArrayObject>().containsPure(
            ctx.frameMgr.cxForLocalUseOnly()->names().raw));
        VIRTPUSH(StackVal(ObjectValue(*cso)));
        END_OP(CallSiteObj);
      }

      CASE(IsConstructing) {
        VIRTPUSH(StackVal(MagicValue(JS_IS_CONSTRUCTING)));
        END_OP(IsConstructing);
      }

      CASE(SuperFun) {
        JSObject* superEnvFunc = &VIRTPOP().asValue().toObject();
        JSObject* superFun = SuperFunOperation(superEnvFunc);
        VIRTPUSH(StackVal(ObjectOrNullValue(superFun)));
        END_OP(SuperFun);
      }

      CASE(CheckThis) {
        if (VIRTSP(0).asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx));
          GOTO_ERROR();
        }
        END_OP(CheckThis);
      }

      CASE(CheckThisReinit) {
        if (!VIRTSP(0).asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowInitializedThis(cx));
          GOTO_ERROR();
        }
        END_OP(CheckThisReinit);
      }

      CASE(Generator) {
        JSObject* generator;
        {
          PUSH_EXIT_FRAME();
          generator = CreateGeneratorFromFrame(cx, frame);
          if (!generator) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(ObjectValue(*generator)));
        END_OP(Generator);
      }

      CASE(InitialYield) {
        // gen => rval, gen, resumeKind
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &VIRTSP(0).asValue().toObject());
        uint32_t frameSize = ctx.stack.frameSize(sp, frame);
        {
          PUSH_EXIT_FRAME();
          if (!NormalSuspend(cx, obj0, frame, frameSize, pc)) {
            GOTO_ERROR();
          }
        }
        frame->setReturnValue(VIRTSP(0).asValue());
        goto do_return;
      }

      CASE(Await)
      CASE(Yield) {
        // rval1, gen => rval2, gen, resumeKind
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &VIRTPOP().asValue().toObject());
        uint32_t frameSize = ctx.stack.frameSize(sp, frame);
        {
          PUSH_EXIT_FRAME();
          if (!NormalSuspend(cx, obj0, frame, frameSize, pc)) {
            GOTO_ERROR();
          }
        }
        frame->setReturnValue(VIRTSP(0).asValue());
        goto do_return;
      }

      CASE(FinalYieldRval) {
        // gen =>
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &VIRTPOP().asValue().toObject());
        {
          PUSH_EXIT_FRAME();
          if (!FinalSuspend(cx, obj0, pc)) {
            GOTO_ERROR();
          }
        }
        goto do_return;
      }

      CASE(IsGenClosing) {
        bool result = VIRTSP(0).asValue() == MagicValue(JS_GENERATOR_CLOSING);
        VIRTPUSH(StackVal(BooleanValue(result)));
        END_OP(IsGenClosing);
      }

      CASE(AsyncAwait) {
        // value, gen => promise
        JSObject* promise;
        {
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTPOP().asValue().toObject());  // gen
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // value
          PUSH_EXIT_FRAME();
          promise = AsyncFunctionAwait(
              cx, obj0.as<AsyncFunctionGeneratorObject>(), value0);
          if (!promise) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(ObjectValue(*promise)));
        END_OP(AsyncAwait);
      }

      CASE(AsyncResolve) {
        // value, gen => promise
        JSObject* promise;
        {
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTPOP().asValue().toObject());  // gen
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // value
          PUSH_EXIT_FRAME();
          promise = AsyncFunctionResolve(
              cx, obj0.as<AsyncFunctionGeneratorObject>(), value0);
          if (!promise) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(ObjectValue(*promise)));
        END_OP(AsyncResolve);
      }

      CASE(AsyncReject) {
        // reason, gen => promise
        JSObject* promise;
        {
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTPOP().asValue().toObject());  // gen
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // stack
          ReservedRooted<Value> value1(&state.value1,
                                       VIRTPOP().asValue());  // reason
          PUSH_EXIT_FRAME();
          promise = AsyncFunctionReject(
              cx, obj0.as<AsyncFunctionGeneratorObject>(), value1, value0);
          if (!promise) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(ObjectValue(*promise)));
        END_OP(AsyncReject);
      }

      CASE(CanSkipAwait) {
        // value => value, can_skip
        bool result = false;
        {
          ReservedRooted<Value> value0(&state.value0, VIRTSP(0).asValue());
          PUSH_EXIT_FRAME();
          if (!CanSkipAwait(cx, value0, &result)) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(BooleanValue(result)));
        END_OP(CanSkipAwait);
      }

      CASE(MaybeExtractAwaitValue) {
        // value, can_skip => value_or_resolved, can_skip
        {
          Value can_skip = VIRTPOP().asValue();
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTPOP().asValue());  // value
          if (can_skip.toBoolean()) {
            PUSH_EXIT_FRAME();
            if (!ExtractAwaitValue(cx, value0, &value0)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(value0));
          VIRTPUSH(StackVal(can_skip));
        }
        END_OP(MaybeExtractAwaitValue);
      }

      CASE(ResumeKind) {
        GeneratorResumeKind resumeKind = ResumeKindFromPC(pc);
        VIRTPUSH(StackVal(Int32Value(int32_t(resumeKind))));
        END_OP(ResumeKind);
      }

      CASE(CheckResumeKind) {
        // rval, gen, resumeKind => rval
        {
          GeneratorResumeKind resumeKind =
              IntToResumeKind(VIRTPOP().asValue().toInt32());
          ReservedRooted<JSObject*> obj0(
              &state.obj0,
              &VIRTPOP().asValue().toObject());  // gen
          ReservedRooted<Value> value0(&state.value0,
                                       VIRTSP(0).asValue());  // rval
          if (resumeKind != GeneratorResumeKind::Next) {
            PUSH_EXIT_FRAME();
            MOZ_ALWAYS_FALSE(GeneratorThrowOrReturn(
                cx, frame, obj0.as<AbstractGeneratorObject>(), value0,
                resumeKind));
            GOTO_ERROR();
          }
        }
        END_OP(CheckResumeKind);
      }

      CASE(Resume) {
        SYNCSP();
        Value gen = VIRTSP(2).asValue();
        Value* callerSP = reinterpret_cast<Value*>(sp);
        {
          ReservedRooted<Value> value0(&state.value0);
          ReservedRooted<JSObject*> obj0(&state.obj0, &gen.toObject());
          {
            PUSH_EXIT_FRAME();
            TRACE_PRINTF("Going to C++ interp for Resume\n");
            if (!InterpretResume(cx, obj0, callerSP, &value0)) {
              GOTO_ERROR();
            }
          }
          VIRTPOPN(2);
          VIRTSPWRITE(0, StackVal(value0));
        }
        END_OP(Resume);
      }

      CASE(JumpTarget) {
        int32_t icIndex = GET_INT32(pc);
        icEntry = icEntries + icIndex;
        COUNT_COVERAGE_PC(pc);
        END_OP(JumpTarget);
      }
      CASE(LoopHead) {
        int32_t icIndex = GET_INT32(pc);
        icEntry = icEntries + icIndex;
#ifdef ENABLE_INTERRUPT_CHECKS
        if (ctx.frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
          PUSH_EXIT_FRAME();
          if (!InterruptCheck(cx)) {
            GOTO_ERROR();
          }
        }
#endif
        COUNT_COVERAGE_PC(pc);
        END_OP(LoopHead);
      }
      CASE(AfterYield) {
        int32_t icIndex = GET_INT32(pc);
        icEntry = icEntries + icIndex;
        if (frame->script()->isDebuggee()) {
          TRACE_PRINTF("doing DebugAfterYield\n");
          PUSH_EXIT_FRAME();
          ReservedRooted<JSScript*> script0(&state.script0, frame->script());
          if (DebugAPI::hasAnyBreakpointsOrStepMode(script0) &&
              !HandleDebugTrap(cx, frame, pc)) {
            TRACE_PRINTF("HandleDebugTrap returned error\n");
            GOTO_ERROR();
          }
          if (!DebugAfterYield(cx, frame)) {
            TRACE_PRINTF("DebugAfterYield returned error\n");
            GOTO_ERROR();
          }
        }
        COUNT_COVERAGE_PC(pc);
        END_OP(AfterYield);
      }

      CASE(Goto) {
        ADVANCE(GET_JUMP_OFFSET(pc));
        PREDICT_NEXT(JumpTarget);
        PREDICT_NEXT(LoopHead);
        DISPATCH();
      }

      CASE(Coalesce) {
        if (!VIRTSP(0).asValue().isNullOrUndefined()) {
          ADVANCE(GET_JUMP_OFFSET(pc));
          DISPATCH();
        } else {
          END_OP(Coalesce);
        }
      }

      CASE(Case) {
        bool cond = VIRTPOP().asValue().toBoolean();
        if (cond) {
          VIRTPOP();
          ADVANCE(GET_JUMP_OFFSET(pc));
          DISPATCH();
        } else {
          END_OP(Case);
        }
      }

      CASE(Default) {
        VIRTPOP();
        ADVANCE(GET_JUMP_OFFSET(pc));
        DISPATCH();
      }

      CASE(TableSwitch) {
        int32_t len = GET_JUMP_OFFSET(pc);
        int32_t low = GET_JUMP_OFFSET(pc + 1 * JUMP_OFFSET_LEN);
        int32_t high = GET_JUMP_OFFSET(pc + 2 * JUMP_OFFSET_LEN);
        Value v = VIRTPOP().asValue();
        int32_t i = 0;
        if (v.isInt32()) {
          i = v.toInt32();
        } else if (!v.isDouble() ||
                   !mozilla::NumberEqualsInt32(v.toDouble(), &i)) {
          ADVANCE(len);
          DISPATCH();
        }

        if (i >= low && i <= high) {
          uint32_t idx = uint32_t(i) - uint32_t(low);
          uint32_t firstResumeIndex = GET_RESUMEINDEX(pc + 3 * JUMP_OFFSET_LEN);
          pc = entryPC + resumeOffsets[firstResumeIndex + idx];
          DISPATCH();
        }
        ADVANCE(len);
        DISPATCH();
      }

      CASE(Return) {
        frame->setReturnValue(VIRTPOP().asValue());
        goto do_return;
      }

      CASE(GetRval) {
        VIRTPUSH(StackVal(frame->returnValue()));
        END_OP(GetRval);
      }

      CASE(SetRval) {
        frame->setReturnValue(VIRTPOP().asValue());
        END_OP(SetRval);
      }

    do_return:
      CASE(RetRval) {
        SYNCSP();
        bool ok = true;
        if (frame->isDebuggee() && !from_unwind) {
          TRACE_PRINTF("doing DebugEpilogueOnBaselineReturn\n");
          PUSH_EXIT_FRAME();
          ok = DebugEpilogueOnBaselineReturn(cx, frame, pc);
        }
        from_unwind = false;

        uint32_t argc = frame->numActualArgs();
        sp = ctx.stack.popFrame();

        // If FP is higher than the entry frame now, return; otherwise,
        // do an inline state update.
        if (stack.fp > entryFrame) {
          *ret = frame->returnValue();
          TRACE_PRINTF("ret = %" PRIx64 "\n", ret->asRawBits());
          return ok ? PBIResult::Ok : PBIResult::Error;
        } else {
          TRACE_PRINTF("Return fastpath\n");
          Value ret = frame->returnValue();
          TRACE_PRINTF("ret = %" PRIx64 "\n", ret.asRawBits());

          // Pop exit frame as well.
          sp = ctx.stack.popFrame();
          // Pop fake return address and descriptor.
          POPNNATIVE(2);

          // Set PC, frame, and current script.
          frame = reinterpret_cast<BaselineFrame*>(
              reinterpret_cast<uintptr_t>(stack.fp) - BaselineFrame::Size());
          TRACE_PRINTF(" sp -> %p, fp -> %p, frame -> %p\n", sp, ctx.stack.fp,
                       frame);
          ctx.frameMgr.switchToFrame(frame);
          ctx.frame = frame;
          RESET_PC(frame->interpreterPC(), frame->script());

          // Adjust caller's stack to complete the call op that PC still points
          // to in that frame (pop args, push return value).
          JSOp op = JSOp(*pc);
          bool constructing = (op == JSOp::New || op == JSOp::NewContent ||
                               op == JSOp::SuperCall);
          // Fix-up return value; EnterJit would do this if we hadn't bypassed
          // it.
          if (constructing && ret.isPrimitive()) {
            ret = sp[argc + constructing].asValue();
            TRACE_PRINTF("updated ret = %" PRIx64 "\n", ret.asRawBits());
          }
          // Pop args -- this is 1 more than how many are pushed in the
          // `totalArgs` count during the call fastpath because it includes
          // the callee.
          VIRTPOPN(argc + 2 + constructing);
          // Push return value.
          VIRTPUSH(StackVal(ret));

          if (!ok) {
            GOTO_ERROR();
          }

          // Advance past call instruction, and advance past IC.
          NEXT_IC();
          ADVANCE(JSOpLength_Call);

          DISPATCH();
        }
      }

      CASE(CheckReturn) {
        Value thisval = VIRTPOP().asValue();
        // inlined version of frame->checkReturn(thisval, result)
        // (js/src/vm/Stack.cpp).
        HandleValue retVal = frame->returnValue();
        if (retVal.isObject()) {
          VIRTPUSH(StackVal(retVal));
        } else if (!retVal.isUndefined()) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN,
                                            JSDVG_IGNORE_STACK, retVal,
                                            nullptr));
          GOTO_ERROR();
        } else if (thisval.isMagic(JS_UNINITIALIZED_LEXICAL)) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx));
          GOTO_ERROR();
        } else {
          VIRTPUSH(StackVal(thisval));
        }
        END_OP(CheckReturn);
      }

      CASE(Throw) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowOperation(cx, value0));
          GOTO_ERROR();
        }
        END_OP(Throw);
      }

      CASE(ThrowWithStack) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          ReservedRooted<Value> value1(&state.value1, VIRTPOP().asValue());
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowWithStackOperation(cx, value1, value0));
          GOTO_ERROR();
        }
        END_OP(ThrowWithStack);
      }

      CASE(ThrowMsg) {
        {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowMsgOperation(cx, GET_UINT8(pc)));
          GOTO_ERROR();
        }
        END_OP(ThrowMsg);
      }

      CASE(ThrowSetConst) {
        {
          PUSH_EXIT_FRAME();
          ReservedRooted<JSScript*> script0(&state.script0, frame->script());
          ReportRuntimeLexicalError(cx, JSMSG_BAD_CONST_ASSIGN, script0, pc);
          GOTO_ERROR();
        }
        END_OP(ThrowSetConst);
      }

      CASE(Try)
      CASE(TryDestructuring) {
        static_assert(JSOpLength_Try == JSOpLength_TryDestructuring);
        END_OP(Try);
      }

      CASE(Exception) {
        {
          PUSH_EXIT_FRAME();
          if (!GetAndClearException(cx, &state.res)) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(state.res));
        state.res.setUndefined();
        END_OP(Exception);
      }

      CASE(ExceptionAndStack) {
        {
          ReservedRooted<Value> value0(&state.value0);
          {
            PUSH_EXIT_FRAME();
            if (!cx.getCx()->getPendingExceptionStack(&value0)) {
              GOTO_ERROR();
            }
            if (!GetAndClearException(cx, &state.res)) {
              GOTO_ERROR();
            }
          }
          VIRTPUSH(StackVal(state.res));
          VIRTPUSH(StackVal(value0));
          state.res.setUndefined();
        }
        END_OP(ExceptionAndStack);
      }

      CASE(Finally) {
#ifdef ENABLE_INTERRUPT_CHECKS
        if (ctx.frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
          PUSH_EXIT_FRAME();
          if (!InterruptCheck(cx)) {
            GOTO_ERROR();
          }
        }
#endif
        END_OP(Finally);
      }

      CASE(Uninitialized) {
        VIRTPUSH(StackVal(MagicValue(JS_UNINITIALIZED_LEXICAL)));
        END_OP(Uninitialized);
      }
      CASE(InitLexical) {
        uint32_t i = GET_LOCALNO(pc);
        frame->unaliasedLocal(i) = VIRTSP(0).asValue();
        END_OP(InitLexical);
      }

      CASE(InitAliasedLexical) {
        EnvironmentCoordinate ec = EnvironmentCoordinate(pc);
        EnvironmentObject& obj = getEnvironmentFromCoordinate(frame, ec);
        obj.setAliasedBinding(ec, VIRTSP(0).asValue());
        END_OP(InitAliasedLexical);
      }
      CASE(CheckLexical) {
        if (VIRTSP(0).asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
          PUSH_EXIT_FRAME();
          ReservedRooted<JSScript*> script0(&state.script0, frame->script());
          ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script0,
                                    pc);
          GOTO_ERROR();
        }
        END_OP(CheckLexical);
      }
      CASE(CheckAliasedLexical) {
        if (VIRTSP(0).asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
          PUSH_EXIT_FRAME();
          ReservedRooted<JSScript*> script0(&state.script0, frame->script());
          ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script0,
                                    pc);
          GOTO_ERROR();
        }
        END_OP(CheckAliasedLexical);
      }

      CASE(BindUnqualifiedGName) {
        IC_SET_OBJ_ARG(
            0,
            &ctx.frameMgr.cxForLocalUseOnly()->global()->lexicalEnvironment());
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(BindName, false);
        END_OP(BindUnqualifiedGName);
      }
      CASE(BindName) {
        IC_SET_OBJ_ARG(0, frame->environmentChain());
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(BindName, false);
        END_OP(BindName);
      }
      CASE(BindUnqualifiedName) {
        IC_SET_OBJ_ARG(0, frame->environmentChain());
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(BindName, false);
        END_OP(BindUnqualifiedName);
      }
      CASE(GetGName) {
        IC_SET_OBJ_ARG(
            0,
            &ctx.frameMgr.cxForLocalUseOnly()->global()->lexicalEnvironment());
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetName, false);
        END_OP(GetGName);
      }
      CASE(GetName) {
        IC_SET_OBJ_ARG(0, frame->environmentChain());
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetName, false);
        END_OP(GetName);
      }

      CASE(GetArg) {
        unsigned i = GET_ARGNO(pc);
        if (argsObjAliasesFormals) {
          VIRTPUSH(StackVal(frame->argsObj().arg(i)));
        } else {
          VIRTPUSH(StackVal(frame->argv()[i]));
        }
        END_OP(GetArg);
      }

      CASE(GetFrameArg) {
        uint32_t i = GET_ARGNO(pc);
        VIRTPUSH(StackVal(frame->argv()[i]));
        END_OP(GetFrameArg);
      }

      CASE(GetLocal) {
        uint32_t i = GET_LOCALNO(pc);
        TRACE_PRINTF(" -> local: %d\n", int(i));
        VIRTPUSH(StackVal(GETLOCAL(i)));
        END_OP(GetLocal);
      }

      CASE(ArgumentsLength) {
        VIRTPUSH(StackVal(Int32Value(frame->numActualArgs())));
        END_OP(ArgumentsLength);
      }

      CASE(GetActualArg) {
        MOZ_ASSERT(!frame->script()->needsArgsObj());
        uint32_t index = VIRTSP(0).asValue().toInt32();
        VIRTSPWRITE(0, StackVal(frame->unaliasedActual(index)));
        END_OP(GetActualArg);
      }

      CASE(GetAliasedVar)
      CASE(GetAliasedDebugVar) {
        static_assert(JSOpLength_GetAliasedVar ==
                      JSOpLength_GetAliasedDebugVar);
        EnvironmentCoordinate ec = EnvironmentCoordinate(pc);
        EnvironmentObject& obj = getEnvironmentFromCoordinate(frame, ec);
        VIRTPUSH(StackVal(obj.aliasedBinding(ec)));
        END_OP(GetAliasedVar);
      }

      CASE(GetImport) {
        IC_ZERO_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(GetImport, false);
        END_OP(GetImport);
      }

      CASE(GetIntrinsic) {
        IC_ZERO_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(LazyConstant, false);
        END_OP(GetIntrinsic);
      }

      CASE(Callee) {
        VIRTPUSH(StackVal(frame->calleev()));
        END_OP(Callee);
      }

      CASE(EnvCallee) {
        uint8_t numHops = GET_UINT8(pc);
        JSObject* env = &frame->environmentChain()->as<EnvironmentObject>();
        for (unsigned i = 0; i < numHops; i++) {
          env = &env->as<EnvironmentObject>().enclosingEnvironment();
        }
        VIRTPUSH(StackVal(ObjectValue(env->as<CallObject>().callee())));
        END_OP(EnvCallee);
      }

      CASE(SetProp)
      CASE(StrictSetProp)
      CASE(SetName)
      CASE(StrictSetName)
      CASE(SetGName)
      CASE(StrictSetGName) {
        static_assert(JSOpLength_SetProp == JSOpLength_StrictSetProp);
        static_assert(JSOpLength_SetProp == JSOpLength_SetName);
        static_assert(JSOpLength_SetProp == JSOpLength_StrictSetName);
        static_assert(JSOpLength_SetProp == JSOpLength_SetGName);
        static_assert(JSOpLength_SetProp == JSOpLength_StrictSetGName);
        IC_POP_ARG(1);
        IC_POP_ARG(0);
        IC_ZERO_ARG(2);
        VIRTPUSH(StackVal(ic_arg1));
        INVOKE_IC(SetProp, false);
        END_OP(SetProp);
      }

      CASE(InitProp)
      CASE(InitHiddenProp)
      CASE(InitLockedProp) {
        static_assert(JSOpLength_InitProp == JSOpLength_InitHiddenProp);
        static_assert(JSOpLength_InitProp == JSOpLength_InitLockedProp);
        IC_POP_ARG(1);
        IC_SET_ARG_FROM_STACK(0, 0);
        IC_ZERO_ARG(2);
        INVOKE_IC(SetProp, false);
        END_OP(InitProp);
      }
      CASE(InitGLexical) {
        IC_SET_ARG_FROM_STACK(1, 0);
        IC_SET_OBJ_ARG(
            0,
            &ctx.frameMgr.cxForLocalUseOnly()->global()->lexicalEnvironment());
        IC_ZERO_ARG(2);
        INVOKE_IC(SetProp, false);
        END_OP(InitGLexical);
      }

      CASE(SetArg) {
        unsigned i = GET_ARGNO(pc);
        if (argsObjAliasesFormals) {
          frame->argsObj().setArg(i, VIRTSP(0).asValue());
        } else {
          frame->argv()[i] = VIRTSP(0).asValue();
        }
        END_OP(SetArg);
      }

      CASE(SetLocal) {
        uint32_t i = GET_LOCALNO(pc);
        TRACE_PRINTF(" -> local: %d\n", int(i));
        SETLOCAL(i, VIRTSP(0).asValue());
        END_OP(SetLocal);
      }

      CASE(SetAliasedVar) {
        EnvironmentCoordinate ec = EnvironmentCoordinate(pc);
        EnvironmentObject& obj = getEnvironmentFromCoordinate(frame, ec);
        MOZ_ASSERT(!IsUninitializedLexical(obj.aliasedBinding(ec)));
        obj.setAliasedBinding(ec, VIRTSP(0).asValue());
        END_OP(SetAliasedVar);
      }

      CASE(SetIntrinsic) {
        {
          ReservedRooted<Value> value0(&state.value0, VIRTSP(0).asValue());
          ReservedRooted<JSScript*> script0(&state.script0, frame->script());
          {
            PUSH_EXIT_FRAME();
            if (!SetIntrinsicOperation(cx, script0, pc, value0)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(SetIntrinsic);
      }

      CASE(PushLexicalEnv) {
        {
          ReservedRooted<Scope*> scope0(&state.scope0,
                                        frame->script()->getScope(pc));
          {
            PUSH_EXIT_FRAME();
            if (!frame->pushLexicalEnvironment(cx, scope0.as<LexicalScope>())) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(PushLexicalEnv);
      }
      CASE(PopLexicalEnv) {
        if (frame->isDebuggee()) {
          TRACE_PRINTF("doing DebugLeaveThenPopLexicalEnv\n");
          PUSH_EXIT_FRAME();
          if (!DebugLeaveThenPopLexicalEnv(cx, frame, pc)) {
            GOTO_ERROR();
          }
        } else {
          frame->popOffEnvironmentChain<LexicalEnvironmentObject>();
        }
        END_OP(PopLexicalEnv);
      }
      CASE(DebugLeaveLexicalEnv) {
        if (frame->isDebuggee()) {
          TRACE_PRINTF("doing DebugLeaveLexicalEnv\n");
          PUSH_EXIT_FRAME();
          if (!DebugLeaveLexicalEnv(cx, frame, pc)) {
            GOTO_ERROR();
          }
        }
        END_OP(DebugLeaveLexicalEnv);
      }

      CASE(RecreateLexicalEnv) {
        {
          PUSH_EXIT_FRAME();
          if (frame->isDebuggee()) {
            TRACE_PRINTF("doing DebuggeeRecreateLexicalEnv\n");
            if (!DebuggeeRecreateLexicalEnv(cx, frame, pc)) {
              GOTO_ERROR();
            }
          } else {
            if (!frame->recreateLexicalEnvironment<false>(cx)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(RecreateLexicalEnv);
      }

      CASE(FreshenLexicalEnv) {
        {
          PUSH_EXIT_FRAME();
          if (frame->isDebuggee()) {
            TRACE_PRINTF("doing DebuggeeFreshenLexicalEnv\n");
            if (!DebuggeeFreshenLexicalEnv(cx, frame, pc)) {
              GOTO_ERROR();
            }
          } else {
            if (!frame->freshenLexicalEnvironment<false>(cx)) {
              GOTO_ERROR();
            }
          }
        }
        END_OP(FreshenLexicalEnv);
      }
      CASE(PushClassBodyEnv) {
        {
          ReservedRooted<Scope*> scope0(&state.scope0,
                                        frame->script()->getScope(pc));
          PUSH_EXIT_FRAME();
          if (!frame->pushClassBodyEnvironment(cx,
                                               scope0.as<ClassBodyScope>())) {
            GOTO_ERROR();
          }
        }
        END_OP(PushClassBodyEnv);
      }
      CASE(PushVarEnv) {
        {
          ReservedRooted<Scope*> scope0(&state.scope0,
                                        frame->script()->getScope(pc));
          PUSH_EXIT_FRAME();
          if (!frame->pushVarEnvironment(cx, scope0)) {
            GOTO_ERROR();
          }
        }
        END_OP(PushVarEnv);
      }
      CASE(EnterWith) {
        {
          ReservedRooted<Scope*> scope0(&state.scope0,
                                        frame->script()->getScope(pc));
          ReservedRooted<Value> value0(&state.value0, VIRTPOP().asValue());
          PUSH_EXIT_FRAME();
          if (!EnterWithOperation(cx, frame, value0, scope0.as<WithScope>())) {
            GOTO_ERROR();
          }
        }
        END_OP(EnterWith);
      }
      CASE(LeaveWith) {
        frame->popOffEnvironmentChain<WithEnvironmentObject>();
        END_OP(LeaveWith);
      }
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      CASE(AddDisposable) {
        {
          ReservedRooted<JSObject*> env(&state.obj0, frame->environmentChain());

          ReservedRooted<JS::Value> needsClosure(&state.value0,
                                                 VIRTPOP().asValue());
          ReservedRooted<JS::Value> method(&state.value1, VIRTPOP().asValue());
          ReservedRooted<JS::Value> val(&state.value2, VIRTPOP().asValue());
          UsingHint hint = UsingHint(GET_UINT8(pc));
          PUSH_EXIT_FRAME();
          if (!AddDisposableResourceToCapability(
                  cx, env, val, method, needsClosure.toBoolean(), hint)) {
            GOTO_ERROR();
          }
        }
        END_OP(AddDisposable);
      }

      CASE(TakeDisposeCapability) {
        {
          ReservedRooted<JSObject*> env(&state.obj0, frame->environmentChain());
          JS::Value maybeDisposables =
              env->as<DisposableEnvironmentObject>().getDisposables();

          MOZ_ASSERT(maybeDisposables.isObject() ||
                     maybeDisposables.isUndefined());

          if (maybeDisposables.isUndefined()) {
            VIRTPUSH(StackVal(UndefinedValue()));
          } else {
            VIRTPUSH(StackVal(maybeDisposables));
            env->as<DisposableEnvironmentObject>().clearDisposables();
          }
        }
        END_OP(TakeDisposeCapability);
      }

      CASE(CreateSuppressedError) {
        ErrorObject* errorObj;
        {
          ReservedRooted<JS::Value> error(&state.value0, VIRTPOP().asValue());
          ReservedRooted<JS::Value> suppressed(&state.value1,
                                               VIRTPOP().asValue());
          PUSH_EXIT_FRAME();
          errorObj = CreateSuppressedError(cx, error, suppressed);
          if (!errorObj) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(ObjectValue(*errorObj)));
        END_OP(CreateSuppressedError);
      }
#endif
      CASE(BindVar) {
        JSObject* varObj;
        {
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         frame->environmentChain());
          PUSH_EXIT_FRAME();
          varObj = BindVarOperation(cx, obj0);
        }
        VIRTPUSH(StackVal(ObjectValue(*varObj)));
        END_OP(BindVar);
      }

      CASE(GlobalOrEvalDeclInstantiation) {
        GCThingIndex lastFun = GET_GCTHING_INDEX(pc);
        {
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         frame->environmentChain());
          ReservedRooted<JSScript*> script0(&state.script0, frame->script());
          PUSH_EXIT_FRAME();
          if (!GlobalOrEvalDeclInstantiation(cx, obj0, script0, lastFun)) {
            GOTO_ERROR();
          }
        }
        END_OP(GlobalOrEvalDeclInstantiation);
      }

      CASE(DelName) {
        {
          ReservedRooted<PropertyName*> name0(&state.name0,
                                              frame->script()->getName(pc));
          ReservedRooted<JSObject*> obj0(&state.obj0,
                                         frame->environmentChain());
          PUSH_EXIT_FRAME();
          if (!DeleteNameOperation(cx, name0, obj0, &state.res)) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(state.res));
        state.res.setUndefined();
        END_OP(DelName);
      }

      CASE(Arguments) {
        {
          PUSH_EXIT_FRAME();
          if (!NewArgumentsObject(cx, frame, &state.res)) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(state.res));
        state.res.setUndefined();
        END_OP(Arguments);
      }

      CASE(Rest) {
        IC_ZERO_ARG(0);
        IC_ZERO_ARG(1);
        IC_ZERO_ARG(2);
        INVOKE_IC_AND_PUSH(Rest, false);
        END_OP(Rest);
      }

      CASE(FunctionThis) {
        {
          PUSH_EXIT_FRAME();
          if (!js::GetFunctionThis(cx, frame, &state.res)) {
            GOTO_ERROR();
          }
        }
        VIRTPUSH(StackVal(state.res));
        state.res.setUndefined();
        END_OP(FunctionThis);
      }

      CASE(Pop) {
        VIRTPOP();
        END_OP(Pop);
      }
      CASE(PopN) {
        SYNCSP();
        uint32_t n = GET_UINT16(pc);
        VIRTPOPN(n);
        END_OP(PopN);
      }
      CASE(Dup) {
        StackVal value = VIRTSP(0);
        VIRTPUSH(value);
        END_OP(Dup);
      }
      CASE(Dup2) {
        StackVal value1 = VIRTSP(0);
        StackVal value2 = VIRTSP(1);
        VIRTPUSH(value2);
        VIRTPUSH(value1);
        END_OP(Dup2);
      }
      CASE(DupAt) {
        unsigned i = GET_UINT24(pc);
        StackVal value = VIRTSP(i);
        VIRTPUSH(value);
        END_OP(DupAt);
      }
      CASE(Swap) {
        StackVal v0 = VIRTSP(0);
        StackVal v1 = VIRTSP(1);
        VIRTSPWRITE(0, v1);
        VIRTSPWRITE(1, v0);
        END_OP(Swap);
      }
      CASE(Pick) {
        unsigned i = GET_UINT8(pc);
        SYNCSP();
        StackVal tmp = sp[i];
        memmove(&sp[1], &sp[0], sizeof(StackVal) * i);
        VIRTSPWRITE(0, tmp);
        END_OP(Pick);
      }
      CASE(Unpick) {
        unsigned i = GET_UINT8(pc);
        StackVal tmp = VIRTSP(0);
        SYNCSP();
        memmove(&sp[0], &sp[1], sizeof(StackVal) * i);
        sp[i] = tmp;
        END_OP(Unpick);
      }
      CASE(DebugCheckSelfHosted) {
        HandleValue val = SPHANDLE(0);
        {
          PUSH_EXIT_FRAME();
          if (!Debug_CheckSelfHosted(cx, val)) {
            GOTO_ERROR();
          }
        }
        END_OP(DebugCheckSelfHosted);
      }
      CASE(Lineno) { END_OP(Lineno); }
      CASE(NopDestructuring) { END_OP(NopDestructuring); }
      CASE(ForceInterpreter) { END_OP(ForceInterpreter); }
      CASE(Debugger) {
        {
          PUSH_EXIT_FRAME();
          if (!OnDebuggerStatement(cx, frame)) {
            GOTO_ERROR();
          }
        }
        END_OP(Debugger);
      }

    label_default:
#ifndef ENABLE_COMPUTED_GOTO_DISPATCH
    default:
#endif
      MOZ_CRASH("Bad opcode");
    }
  }

restart:
  // This is a `goto` target so that we exit any on-stack exit frames
  // before restarting, to match previous behavior.
  return PortableBaselineInterpret<true, HybridICs>(
      ctx.frameMgr.cxForLocalUseOnly(), ctx.state, ctx.stack, sp, envChain, ret,
      pc, isd, entryPC, frame, entryFrame, restartCode);

error:
  TRACE_PRINTF("HandleException: frame %p\n", frame);
  {
    ResumeFromException rfe;
    {
      PUSH_EXIT_FRAME();
      HandleException(&rfe);
    }

    switch (rfe.kind) {
      case ExceptionResumeKind::EntryFrame:
        TRACE_PRINTF(" -> Return from entry frame\n");
        frame->setReturnValue(MagicValue(JS_ION_ERROR));
        ctx.stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        ctx.stack.unwindingSP = reinterpret_cast<StackVal*>(rfe.stackPointer);
        ctx.stack.unwindingFP = reinterpret_cast<StackVal*>(rfe.framePointer);
        goto unwind_error;
      case ExceptionResumeKind::Catch:
        RESET_PC(frame->interpreterPC(), frame->script());
        ctx.stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        ctx.stack.unwindingSP = reinterpret_cast<StackVal*>(rfe.stackPointer);
        ctx.stack.unwindingFP = reinterpret_cast<StackVal*>(rfe.framePointer);
        TRACE_PRINTF(" -> catch to pc %p\n", pc);
        goto unwind;
      case ExceptionResumeKind::Finally:
        RESET_PC(frame->interpreterPC(), frame->script());
        ctx.stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        sp = reinterpret_cast<StackVal*>(rfe.stackPointer);
        TRACE_PRINTF(" -> finally to pc %p\n", pc);
        VIRTPUSH(StackVal(rfe.exception));
        VIRTPUSH(StackVal(rfe.exceptionStack));
        VIRTPUSH(StackVal(BooleanValue(true)));
        ctx.stack.unwindingSP = sp;
        ctx.stack.unwindingFP = ctx.stack.fp;
        goto unwind;
      case ExceptionResumeKind::ForcedReturnBaseline:
        RESET_PC(frame->interpreterPC(), frame->script());
        ctx.stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        ctx.stack.unwindingSP = reinterpret_cast<StackVal*>(rfe.stackPointer);
        ctx.stack.unwindingFP = reinterpret_cast<StackVal*>(rfe.framePointer);
        TRACE_PRINTF(" -> forced return\n");
        goto unwind_ret;
      case ExceptionResumeKind::ForcedReturnIon:
        MOZ_CRASH(
            "Unexpected ForcedReturnIon exception-resume kind in Portable "
            "Baseline");
      case ExceptionResumeKind::Bailout:
        MOZ_CRASH(
            "Unexpected Bailout exception-resume kind in Portable Baseline");
      case ExceptionResumeKind::WasmInterpEntry:
        MOZ_CRASH(
            "Unexpected WasmInterpEntry exception-resume kind in Portable "
            "Baseline");
      case ExceptionResumeKind::WasmCatch:
        MOZ_CRASH(
            "Unexpected WasmCatch exception-resume kind in Portable "
            "Baseline");
    }
  }

  DISPATCH();

ic_fail:
  RESTART(ic_result);
  switch (ic_result) {
    case PBIResult::Ok:
      MOZ_CRASH("Unreachable: ic_result must be an error if we reach ic_fail");
    case PBIResult::Error:
      goto error;
    case PBIResult::Unwind:
      goto unwind;
    case PBIResult::UnwindError:
      goto unwind_error;
    case PBIResult::UnwindRet:
      goto unwind_ret;
  }

unwind:
  TRACE_PRINTF("unwind: fp = %p entryFrame = %p\n", ctx.stack.fp, entryFrame);
  if (reinterpret_cast<uintptr_t>(ctx.stack.unwindingFP) >
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    TRACE_PRINTF(" -> returning\n");
    return PBIResult::Unwind;
  }
  sp = ctx.stack.unwindingSP;
  ctx.stack.fp = ctx.stack.unwindingFP;
  frame = reinterpret_cast<BaselineFrame*>(
      reinterpret_cast<uintptr_t>(ctx.stack.fp) - BaselineFrame::Size());
  TRACE_PRINTF(" -> setting sp to %p, frame to %p\n", sp, frame);
  ctx.frameMgr.switchToFrame(frame);
  ctx.frame = frame;
  RESET_PC(frame->interpreterPC(), frame->script());
  DISPATCH();
unwind_error:
  TRACE_PRINTF("unwind_error: fp = %p entryFrame = %p\n", ctx.stack.fp,
               entryFrame);
  if (reinterpret_cast<uintptr_t>(ctx.stack.unwindingFP) >
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    return PBIResult::UnwindError;
  }
  if (reinterpret_cast<uintptr_t>(ctx.stack.unwindingFP) ==
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    return PBIResult::Error;
  }
  sp = ctx.stack.unwindingSP;
  ctx.stack.fp = ctx.stack.unwindingFP;
  frame = reinterpret_cast<BaselineFrame*>(
      reinterpret_cast<uintptr_t>(ctx.stack.fp) - BaselineFrame::Size());
  TRACE_PRINTF(" -> setting sp to %p, frame to %p\n", sp, frame);
  ctx.frameMgr.switchToFrame(frame);
  ctx.frame = frame;
  RESET_PC(frame->interpreterPC(), frame->script());
  goto error;
unwind_ret:
  TRACE_PRINTF("unwind_ret: fp = %p entryFrame = %p\n", ctx.stack.fp,
               entryFrame);
  if (reinterpret_cast<uintptr_t>(ctx.stack.unwindingFP) >
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    return PBIResult::UnwindRet;
  }
  if (reinterpret_cast<uintptr_t>(ctx.stack.unwindingFP) ==
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    *ret = frame->returnValue();
    return PBIResult::Ok;
  }
  sp = ctx.stack.unwindingSP;
  ctx.stack.fp = ctx.stack.unwindingFP;
  frame = reinterpret_cast<BaselineFrame*>(
      reinterpret_cast<uintptr_t>(ctx.stack.fp) - BaselineFrame::Size());
  TRACE_PRINTF(" -> setting sp to %p, frame to %p\n", sp, frame);
  ctx.frameMgr.switchToFrame(frame);
  ctx.frame = frame;
  RESET_PC(frame->interpreterPC(), frame->script());
  from_unwind = true;
  goto do_return;

#ifndef __wasi__
debug:;
  {
    TRACE_PRINTF("hit debug point\n");
    PUSH_EXIT_FRAME();
    if (!HandleDebugTrap(cx, frame, pc)) {
      TRACE_PRINTF("HandleDebugTrap returned error\n");
      goto error;
    }
    RESET_PC(frame->interpreterPC(), frame->script());
    TRACE_PRINTF("HandleDebugTrap done\n");
  }
  goto dispatch;
#endif
}

/*
 * -----------------------------------------------
 * Entry point
 * -----------------------------------------------
 */

bool PortableBaselineTrampoline(JSContext* cx, size_t argc, Value* argv,
                                size_t numFormals, size_t numActuals,
                                CalleeToken calleeToken, JSObject* envChain,
                                Value* result) {
  State state(cx);
  Stack stack(cx->portableBaselineStack());
  StackVal* sp = stack.top;

  TRACE_PRINTF("Trampoline: calleeToken %p env %p\n", calleeToken, envChain);

  // Expected stack frame:
  // - argN
  // - ...
  // - arg1
  // - this
  // - calleeToken
  // - descriptor
  // - "return address" (nullptr for top frame)

  // `argc` is the number of args *including* `this` (`N + 1`
  // above). `numFormals` is the minimum `N`; if less, we need to push
  // `UndefinedValue`s above. We need to pass an argc (including
  // `this`) accoundint for the extra undefs in the descriptor's argc.
  //
  // If constructing, there is an additional `newTarget` at the end.
  //
  // Note that `callee`, which is in the stack signature for a `Call`
  // JSOp, does *not* appear in this count: it is separately passed in
  // the `calleeToken`.

  bool constructing = CalleeTokenIsConstructing(calleeToken);
  size_t numCalleeActuals = std::max(numActuals, numFormals);
  size_t numUndefs = numCalleeActuals - numActuals;

  // N.B.: we already checked the stack in
  // PortableBaselineInterpreterStackCheck; we don't do it here
  // because we can't push an exit frame if we don't have an entry
  // frame, and we need a full activation to produce the backtrace
  // from ReportOverRecursed.

  if (constructing) {
    PUSH(StackVal(argv[argc]));
  }
  for (size_t i = 0; i < numUndefs; i++) {
    PUSH(StackVal(UndefinedValue()));
  }
  for (size_t i = 0; i < argc; i++) {
    PUSH(StackVal(argv[argc - 1 - i]));
  }
  PUSHNATIVE(StackValNative(calleeToken));
  PUSHNATIVE(StackValNative(
      MakeFrameDescriptorForJitCall(FrameType::CppToJSJit, numActuals)));

  JSScript* script = ScriptFromCalleeToken(calleeToken);
  jsbytecode* pc = script->code();
  ImmutableScriptData* isd = script->immutableScriptData();
  PBIResult ret;
  ret = PortableBaselineInterpret<false, kHybridICsInterp>(
      cx, state, stack, sp, envChain, result, pc, isd, nullptr, nullptr,
      nullptr, PBIResult::Ok);
  switch (ret) {
    case PBIResult::Ok:
    case PBIResult::UnwindRet:
      TRACE_PRINTF("PBI returned Ok/UnwindRet with result %" PRIx64 "\n",
                   result->asRawBits());
      break;
    case PBIResult::Error:
    case PBIResult::UnwindError:
      TRACE_PRINTF("PBI returned Error/UnwindError\n");
      return false;
    case PBIResult::Unwind:
      MOZ_CRASH("Should not unwind out of top / entry frame");
  }

  return true;
}

MethodStatus CanEnterPortableBaselineInterpreter(JSContext* cx,
                                                 RunState& state) {
  if (!JitOptions.portableBaselineInterpreter) {
    return MethodStatus::Method_CantCompile;
  }
  if (state.script()->hasJitScript()) {
    return MethodStatus::Method_Compiled;
  }
  if (state.script()->hasForceInterpreterOp()) {
    return MethodStatus::Method_CantCompile;
  }
  if (state.script()->isAsync() || state.script()->isGenerator()) {
    return MethodStatus::Method_CantCompile;
  }
  if (cx->runtime()->geckoProfiler().enabled()) {
    return MethodStatus::Method_CantCompile;
  }

  if (state.isInvoke()) {
    InvokeState& invoke = *state.asInvoke();
    if (TooManyActualArguments(invoke.args().length())) {
      return MethodStatus::Method_CantCompile;
    }
  } else {
    if (state.asExecute()->isDebuggerEval()) {
      return MethodStatus::Method_CantCompile;
    }
  }
  if (state.script()->getWarmUpCount() <=
      JitOptions.portableBaselineInterpreterWarmUpThreshold) {
    return MethodStatus::Method_Skipped;
  }
  if (!cx->zone()->ensureJitZoneExists(cx)) {
    return MethodStatus::Method_Error;
  }

  AutoKeepJitScripts keepJitScript(cx);
  if (!state.script()->ensureHasJitScript(cx, keepJitScript)) {
    return MethodStatus::Method_Error;
  }
  state.script()->updateJitCodeRaw(cx->runtime());
  return MethodStatus::Method_Compiled;
}

bool PortablebaselineInterpreterStackCheck(JSContext* cx, RunState& state,
                                           size_t numActualArgs) {
  auto& pbs = cx->portableBaselineStack();
  StackVal* base = reinterpret_cast<StackVal*>(pbs.base);
  StackVal* top = reinterpret_cast<StackVal*>(pbs.top);
  ssize_t margin = kStackMargin / sizeof(StackVal);
  ssize_t needed = numActualArgs + state.script()->nslots() + margin;
  return (top - base) >= needed;
}

}  // namespace pbl
}  // namespace js
