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

#include "jsapi.h"

#include "builtin/DataViewObject.h"
#include "builtin/MapObject.h"
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
#include "vm/AsyncFunction.h"
#include "vm/AsyncIteration.h"
#include "vm/EnvironmentObject.h"
#include "vm/EqualityOperations.h"
#include "vm/GeneratorObject.h"
#include "vm/Interpreter.h"
#include "vm/Iteration.h"
#include "vm/JitActivation.h"
#include "vm/JSScript.h"
#include "vm/Opcodes.h"
#include "vm/PlainObject.h"
#include "vm/Shape.h"
#include "vm/TypeofEqOperand.h"  // TypeofEqOperand

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

// Whether we are using the "hybrid" strategy for ICs (see the [SMDOC]
// in PortableBaselineInterpret.h for more). This is currently a
// constant, but may become configurable in the future.
static const bool kHybridICs = true;

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

  explicit Stack(PortableBaselineStack& pbs)
      : fp(reinterpret_cast<StackVal*>(pbs.top)),
        base(reinterpret_cast<StackVal*>(pbs.base)),
        top(reinterpret_cast<StackVal*>(pbs.top)),
        unwindingSP(nullptr) {}

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
    frame->setInterpreterFields(script->code());
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
  CacheIRReader cacheIRReader;
  static const int kMaxICVals = 16;
  uint64_t icVals[kMaxICVals];
  uint64_t icResult;
  int extraArgs;
  bool spreadCall;

  ICRegs() : cacheIRReader(nullptr, nullptr) {}
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
  VMFrame(VMFrameManager& mgr, Stack& stack_, StackVal* sp, jsbytecode* pc)
      : cx(mgr.cx), stack(stack_) {
    mgr.frame->interpreterPC() = pc;
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

#define PUSH_EXIT_FRAME_OR_RET(value)                           \
  VMFrame cx(frameMgr, stack, sp, pc);                          \
  if (!cx.success()) {                                          \
    return value;                                               \
  }                                                             \
  StackVal* sp = cx.spBelowFrame(); /* shadow the definition */ \
  (void)sp;                         /* avoid unused-variable warnings */

#define PUSH_IC_FRAME() PUSH_EXIT_FRAME_OR_RET(ICInterpretOpResult::Error)
#define PUSH_FALLBACK_IC_FRAME() PUSH_EXIT_FRAME_OR_RET(PBIResult::Error)
#define PUSH_EXIT_FRAME() PUSH_EXIT_FRAME_OR_RET(PBIResult::Error)

/*
 * -----------------------------------------------
 * IC Interpreter
 * -----------------------------------------------
 */

ICInterpretOpResult MOZ_ALWAYS_INLINE
ICInterpretOps(BaselineFrame* frame, VMFrameManager& frameMgr, State& state,
               ICRegs& icregs, Stack& stack, StackVal* sp, ICCacheIRStub* cstub,
               jsbytecode* pc) {
#define CACHEOP_CASE(name)                                                   \
  cacheop_##name                                                             \
      : TRACE_PRINTF("cacheop (frame %p pc %p stub %p): " #name "\n", frame, \
                     pc, cstub);
#define CACHEOP_CASE_FALLTHROUGH(name) CACHEOP_CASE(name)
#define CACHEOP_CASE_UNIMPL(name) cacheop_##name:

  static const void* const addresses[long(CacheOp::NumOpcodes)] = {
#define OP(name, ...) &&cacheop_##name,
      CACHE_IR_OPS(OP)
#undef OP
  };

#define DISPATCH_CACHEOP()                 \
  cacheop = icregs.cacheIRReader.readOp(); \
  goto* addresses[long(cacheop)];

// We set a fixed bound on the number of icVals which is smaller than what IC
// generators may use. As a result we can't evaluate an IC if it defines too
// many values. Note that we don't need to check this when reading from icVals
// because we should have bailed out before the earlier write which defined the
// same value. Similarly, we don't need to check writes to locations which we've
// just read from.
#define BOUNDSCHECK(resultId) \
  if (resultId.id() >= ICRegs::kMaxICVals) return ICInterpretOpResult::NextIC;

#define PREDICT_NEXT(name)                              \
  if (icregs.cacheIRReader.peekOp() == CacheOp::name) { \
    icregs.cacheIRReader.readOp();                      \
    goto cacheop_##name;                                \
  }

#define PREDICT_RETURN()                                        \
  if (icregs.cacheIRReader.peekOp() == CacheOp::ReturnFromIC) { \
    TRACE_PRINTF("stub successful, predicted return\n");        \
    return ICInterpretOpResult::Return;                         \
  }

  CacheOp cacheop;

  DISPATCH_CACHEOP();

  CACHEOP_CASE(ReturnFromIC) {
    TRACE_PRINTF("stub successful!\n");
    return ICInterpretOpResult::Return;
  }

  CACHEOP_CASE(GuardToObject) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    TRACE_PRINTF("GuardToObject: icVal %" PRIx64 "\n",
                 icregs.icVals[inputId.id()]);
    if (!v.isObject()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[inputId.id()] = reinterpret_cast<uint64_t>(&v.toObject());
    PREDICT_NEXT(GuardShape);
    PREDICT_NEXT(GuardSpecificFunction);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNullOrUndefined) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isNullOrUndefined()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNull) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isNull()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsUndefined) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isUndefined()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNotUninitializedLexical) {
    ValOperandId valId = icregs.cacheIRReader.valOperandId();
    Value val = Value::fromRawBits(icregs.icVals[valId.id()]);
    if (val == MagicValue(JS_UNINITIALIZED_LEXICAL)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToBoolean) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isBoolean()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[inputId.id()] = v.toBoolean() ? 1 : 0;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToString) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isString()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[inputId.id()] = reinterpret_cast<uint64_t>(v.toString());
    PREDICT_NEXT(GuardToString);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToSymbol) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isSymbol()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[inputId.id()] = reinterpret_cast<uint64_t>(v.toSymbol());
    PREDICT_NEXT(GuardSpecificSymbol);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToBigInt) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isBigInt()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[inputId.id()] = reinterpret_cast<uint64_t>(v.toBigInt());
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNumber) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isNumber()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToInt32) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    TRACE_PRINTF("GuardToInt32 (%d): icVal %" PRIx64 "\n", inputId.id(),
                 icregs.icVals[inputId.id()]);
    if (!v.isInt32()) {
      return ICInterpretOpResult::NextIC;
    }
    // N.B.: we don't need to unbox because the low 32 bits are
    // already the int32 itself, and we are careful when using
    // `Int32Operand`s to only use those bits.

    PREDICT_NEXT(GuardToInt32);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToNonGCThing) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value input = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (input.isGCThing()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardBooleanToInt32) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    Value v = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (!v.isBoolean()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[resultId.id()] = v.toBoolean() ? 1 : 0;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToInt32Index) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    Value val = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (val.isInt32()) {
      icregs.icVals[resultId.id()] = val.toInt32();
      DISPATCH_CACHEOP();
    } else if (val.isDouble()) {
      double doubleVal = val.toDouble();
      if (doubleVal >= double(INT32_MIN) && doubleVal <= double(INT32_MAX)) {
        icregs.icVals[resultId.id()] = int32_t(doubleVal);
        DISPATCH_CACHEOP();
      }
    }
    return ICInterpretOpResult::NextIC;
  }

  CACHEOP_CASE(Int32ToIntPtr) {
    Int32OperandId inputId = icregs.cacheIRReader.int32OperandId();
    IntPtrOperandId resultId = icregs.cacheIRReader.intPtrOperandId();
    BOUNDSCHECK(resultId);
    int32_t input = int32_t(icregs.icVals[inputId.id()]);
    // Note that this must sign-extend to pointer width:
    icregs.icVals[resultId.id()] = intptr_t(input);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardToInt32ModUint32) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    Value input = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (input.isInt32()) {
      icregs.icVals[resultId.id()] = Int32Value(input.toInt32()).asRawBits();
      DISPATCH_CACHEOP();
    } else if (input.isDouble()) {
      double doubleVal = input.toDouble();
      // Accept any double that fits in an int64_t but truncate the top 32 bits.
      if (doubleVal >= double(INT64_MIN) && doubleVal <= double(INT64_MAX)) {
        icregs.icVals[resultId.id()] =
            Int32Value(int64_t(doubleVal)).asRawBits();
        DISPATCH_CACHEOP();
      }
    }
    return ICInterpretOpResult::NextIC;
  }

  CACHEOP_CASE(GuardNonDoubleType) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    ValueType type = icregs.cacheIRReader.valueType();
    Value val = Value::fromRawBits(icregs.icVals[inputId.id()]);
    switch (type) {
      case ValueType::String:
        if (!val.isString()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case ValueType::Symbol:
        if (!val.isSymbol()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case ValueType::BigInt:
        if (!val.isBigInt()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case ValueType::Int32:
        if (!val.isInt32()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case ValueType::Boolean:
        if (!val.isBoolean()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case ValueType::Undefined:
        if (!val.isUndefined()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case ValueType::Null:
        if (!val.isNull()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      default:
        MOZ_CRASH("Unexpected type");
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardShape) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t shapeOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uintptr_t expectedShape =
        cstub->stubInfo()->getStubRawWord(cstub, shapeOffset);
    if (reinterpret_cast<uintptr_t>(obj->shape()) != expectedShape) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFuse) {
    RealmFuses::FuseIndex fuseIndex = icregs.cacheIRReader.realmFuseIndex();
    if (!frameMgr.cxForLocalUseOnly()
             ->realm()
             ->realmFuses.getFuseByIndex(fuseIndex)
             ->intact()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardProto) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t protoOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSObject* proto = reinterpret_cast<JSObject*>(
        cstub->stubInfo()->getStubRawWord(cstub, protoOffset));
    if (obj->staticPrototype() != proto) {
      return ICInterpretOpResult::NextIC;
    }
    PREDICT_NEXT(LoadProto);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardNullProto) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (obj->taggedProto().raw()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardClass) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    GuardClassKind kind = icregs.cacheIRReader.guardClassKind();
    JSObject* object = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    switch (kind) {
      case GuardClassKind::Array:
        if (object->getClass() != &ArrayObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::PlainObject:
        if (object->getClass() != &PlainObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::FixedLengthArrayBuffer:
        if (object->getClass() != &FixedLengthArrayBufferObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::ResizableArrayBuffer:
        if (object->getClass() != &ResizableArrayBufferObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::FixedLengthSharedArrayBuffer:
        if (object->getClass() != &FixedLengthSharedArrayBufferObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::GrowableSharedArrayBuffer:
        if (object->getClass() != &GrowableSharedArrayBufferObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::FixedLengthDataView:
        if (object->getClass() != &FixedLengthDataViewObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::ResizableDataView:
        if (object->getClass() != &ResizableDataViewObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::MappedArguments:
        if (object->getClass() != &MappedArgumentsObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::UnmappedArguments:
        if (object->getClass() != &UnmappedArgumentsObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::WindowProxy:
        if (object->getClass() !=
            frameMgr.cxForLocalUseOnly()->runtime()->maybeWindowProxyClass()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::JSFunction:
        if (!object->is<JSFunction>()) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::Set:
        if (object->getClass() != &SetObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::Map:
        if (object->getClass() != &MapObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
      case GuardClassKind::BoundFunction:
        if (object->getClass() != &BoundFunctionObject::class_) {
          return ICInterpretOpResult::NextIC;
        }
        break;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardAnyClass) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t claspOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSClass* clasp = reinterpret_cast<JSClass*>(
        cstub->stubInfo()->getStubRawWord(cstub, claspOffset));
    if (obj->getClass() != clasp) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardGlobalGeneration) {
    uint32_t expectedOffset = icregs.cacheIRReader.stubOffset();
    uint32_t generationAddrOffset = icregs.cacheIRReader.stubOffset();
    uint32_t expected =
        cstub->stubInfo()->getStubRawInt32(cstub, expectedOffset);
    uint32_t* generationAddr = reinterpret_cast<uint32_t*>(
        cstub->stubInfo()->getStubRawWord(cstub, generationAddrOffset));
    if (*generationAddr != expected) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(HasClassResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t claspOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSClass* clasp = reinterpret_cast<JSClass*>(
        cstub->stubInfo()->getStubRawWord(cstub, claspOffset));
    icregs.icResult = BooleanValue(obj->getClass() == clasp).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardCompartment) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t globalOffset = icregs.cacheIRReader.stubOffset();
    uint32_t compartmentOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSObject* global = reinterpret_cast<JSObject*>(
        cstub->stubInfo()->getStubRawWord(cstub, globalOffset));
    JS::Compartment* compartment = reinterpret_cast<JS::Compartment*>(
        cstub->stubInfo()->getStubRawWord(cstub, compartmentOffset));
    if (IsDeadProxyObject(global)) {
      return ICInterpretOpResult::NextIC;
    }
    if (obj->compartment() != compartment) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsExtensible) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (obj->nonProxyIsExtensible()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNativeObject) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (!obj->is<NativeObject>()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsProxy) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (!obj->is<ProxyObject>()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNotProxy) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (obj->is<ProxyObject>()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNotArrayBufferMaybeShared) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    const JSClass* clasp = obj->getClass();
    if (clasp == &ArrayBufferObject::protoClass_ ||
        clasp == &SharedArrayBufferObject::protoClass_) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsTypedArray) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (!IsTypedArrayClass(obj->getClass())) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardHasProxyHandler) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t handlerOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    BaseProxyHandler* handler = reinterpret_cast<BaseProxyHandler*>(
        cstub->stubInfo()->getStubRawWord(cstub, handlerOffset));
    if (obj->as<ProxyObject>().handler() != handler) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardIsNotDOMProxy) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (obj->as<ProxyObject>().handler()->family() ==
        GetDOMProxyHandlerFamily()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardSpecificObject) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t expectedOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSObject* expected = reinterpret_cast<JSObject*>(
        cstub->stubInfo()->getStubRawWord(cstub, expectedOffset));
    if (obj != expected) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardObjectIdentity) {
    ObjOperandId obj1Id = icregs.cacheIRReader.objOperandId();
    ObjOperandId obj2Id = icregs.cacheIRReader.objOperandId();
    JSObject* obj1 = reinterpret_cast<JSObject*>(icregs.icVals[obj1Id.id()]);
    JSObject* obj2 = reinterpret_cast<JSObject*>(icregs.icVals[obj2Id.id()]);
    if (obj1 != obj2) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardSpecificFunction) {
    ObjOperandId funId = icregs.cacheIRReader.objOperandId();
    uint32_t expectedOffset = icregs.cacheIRReader.stubOffset();
    uint32_t nargsAndFlagsOffset = icregs.cacheIRReader.stubOffset();
    (void)nargsAndFlagsOffset;  // Unused.
    uintptr_t expected =
        cstub->stubInfo()->getStubRawWord(cstub, expectedOffset);
    if (expected != icregs.icVals[funId.id()]) {
      return ICInterpretOpResult::NextIC;
    }
    PREDICT_NEXT(LoadArgumentFixedSlot);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFunctionScript) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t expectedOffset = icregs.cacheIRReader.stubOffset();
    uint32_t nargsAndFlagsOffset = icregs.cacheIRReader.stubOffset();
    JSFunction* fun = reinterpret_cast<JSFunction*>(icregs.icVals[objId.id()]);
    BaseScript* expected = reinterpret_cast<BaseScript*>(
        cstub->stubInfo()->getStubRawWord(cstub, expectedOffset));
    (void)nargsAndFlagsOffset;

    if (!fun->hasBaseScript() || fun->baseScript() != expected) {
      return ICInterpretOpResult::NextIC;
    }

    PREDICT_NEXT(CallScriptedFunction);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardSpecificAtom) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    uint32_t expectedOffset = icregs.cacheIRReader.stubOffset();
    uintptr_t expected =
        cstub->stubInfo()->getStubRawWord(cstub, expectedOffset);
    if (expected != icregs.icVals[strId.id()]) {
      // TODO: BaselineCacheIRCompiler also checks for equal strings
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardSpecificSymbol) {
    SymbolOperandId symId = icregs.cacheIRReader.symbolOperandId();
    uint32_t expectedOffset = icregs.cacheIRReader.stubOffset();
    uintptr_t expected =
        cstub->stubInfo()->getStubRawWord(cstub, expectedOffset);
    if (expected != icregs.icVals[symId.id()]) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardSpecificInt32) {
    Int32OperandId numId = icregs.cacheIRReader.int32OperandId();
    int32_t expected = icregs.cacheIRReader.int32Immediate();
    if (expected != int32_t(icregs.icVals[numId.id()])) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardNoDenseElements) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (obj->as<NativeObject>().getDenseInitializedLength() != 0) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardStringToIndex) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    JSString* str = reinterpret_cast<JSString*>(icregs.icVals[strId.id()]);
    int32_t result;
    if (str->hasIndexValue()) {
      uint32_t index = str->getIndexValue();
      MOZ_ASSERT(index <= INT32_MAX);
      result = index;
    } else {
      result = GetIndexFromString(str);
      if (result < 0) {
        return ICInterpretOpResult::NextIC;
      }
    }
    icregs.icVals[resultId.id()] = result;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardStringToInt32) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    JSString* str = reinterpret_cast<JSString*>(icregs.icVals[strId.id()]);
    int32_t result;
    // Use indexed value as fast path if possible.
    if (str->hasIndexValue()) {
      uint32_t index = str->getIndexValue();
      MOZ_ASSERT(index <= INT32_MAX);
      result = index;
    } else {
      if (!GetInt32FromStringPure(frameMgr.cxForLocalUseOnly(), str, &result)) {
        return ICInterpretOpResult::NextIC;
      }
    }
    icregs.icVals[resultId.id()] = result;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardStringToNumber) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    NumberOperandId resultId = icregs.cacheIRReader.numberOperandId();
    BOUNDSCHECK(resultId);
    JSString* str = reinterpret_cast<JSString*>(icregs.icVals[strId.id()]);
    Value result;
    // Use indexed value as fast path if possible.
    if (str->hasIndexValue()) {
      uint32_t index = str->getIndexValue();
      MOZ_ASSERT(index <= INT32_MAX);
      result = Int32Value(index);
    } else {
      double value;
      if (!StringToNumberPure(frameMgr.cxForLocalUseOnly(), str, &value)) {
        return ICInterpretOpResult::NextIC;
      }
      result = DoubleValue(value);
    }
    icregs.icVals[resultId.id()] = result.asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(BooleanToNumber) {
    BooleanOperandId booleanId = icregs.cacheIRReader.booleanOperandId();
    NumberOperandId resultId = icregs.cacheIRReader.numberOperandId();
    BOUNDSCHECK(resultId);
    uint64_t boolean = icregs.icVals[booleanId.id()];
    MOZ_ASSERT((boolean & ~1) == 0);
    icregs.icVals[resultId.id()] = Int32Value(boolean).asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardHasGetterSetter) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t idOffset = icregs.cacheIRReader.stubOffset();
    uint32_t getterSetterOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    jsid id =
        jsid::fromRawBits(cstub->stubInfo()->getStubRawWord(cstub, idOffset));
    GetterSetter* getterSetter = reinterpret_cast<GetterSetter*>(
        cstub->stubInfo()->getStubRawWord(cstub, getterSetterOffset));
    if (!ObjectHasGetterSetterPure(frameMgr.cxForLocalUseOnly(), obj, id,
                                   getterSetter)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardInt32IsNonNegative) {
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    int32_t index = int32_t(icregs.icVals[indexId.id()]);
    if (index < 0) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardDynamicSlotIsSpecificObject) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ObjOperandId expectedId = icregs.cacheIRReader.objOperandId();
    uint32_t slotOffset = icregs.cacheIRReader.stubOffset();
    JSObject* expected =
        reinterpret_cast<JSObject*>(icregs.icVals[expectedId.id()]);
    uintptr_t slot = cstub->stubInfo()->getStubRawInt32(cstub, slotOffset);
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    HeapSlot* slots = nobj->getSlotsUnchecked();
    // Note that unlike similar opcodes, GuardDynamicSlotIsSpecificObject takes
    // a slot index rather than a byte offset.
    Value actual = slots[slot];
    if (actual != ObjectValue(*expected)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardDynamicSlotIsNotObject) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t slotOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t slot = cstub->stubInfo()->getStubRawInt32(cstub, slotOffset);
    NativeObject* nobj = &obj->as<NativeObject>();
    HeapSlot* slots = nobj->getSlotsUnchecked();
    // Note that unlike similar opcodes, GuardDynamicSlotIsNotObject takes a
    // slot index rather than a byte offset.
    Value actual = slots[slot];
    if (actual.isObject()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFixedSlotValue) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    uint32_t valOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    Value val = Value::fromRawBits(
        cstub->stubInfo()->getStubRawInt64(cstub, valOffset));
    GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
        reinterpret_cast<uintptr_t>(obj) + offset);
    Value actual = slot->get();
    if (actual != val) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardDynamicSlotValue) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    uint32_t valOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    Value val = Value::fromRawBits(
        cstub->stubInfo()->getStubRawInt64(cstub, valOffset));
    NativeObject* nobj = &obj->as<NativeObject>();
    HeapSlot* slots = nobj->getSlotsUnchecked();
    Value actual = slots[offset / sizeof(Value)];
    if (actual != val) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadFixedSlot) {
    ValOperandId resultId = icregs.cacheIRReader.valOperandId();
    BOUNDSCHECK(resultId);
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
        reinterpret_cast<uintptr_t>(obj) + offset);
    Value actual = slot->get();
    icregs.icVals[resultId.id()] = actual.asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadDynamicSlot) {
    ValOperandId resultId = icregs.cacheIRReader.valOperandId();
    BOUNDSCHECK(resultId);
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t slotOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t slot = cstub->stubInfo()->getStubRawInt32(cstub, slotOffset);
    NativeObject* nobj = &obj->as<NativeObject>();
    HeapSlot* slots = nobj->getSlotsUnchecked();
    // Note that unlike similar opcodes, LoadDynamicSlot takes a slot index
    // rather than a byte offset.
    Value actual = slots[slot];
    icregs.icVals[resultId.id()] = actual.asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardNoAllocationMetadataBuilder) {
    uint32_t builderAddrOffset = icregs.cacheIRReader.stubOffset();
    uintptr_t builderAddr =
        cstub->stubInfo()->getStubRawWord(cstub, builderAddrOffset);
    if (*reinterpret_cast<uintptr_t*>(builderAddr) != 0) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFunctionHasJitEntry) {
    ObjOperandId funId = icregs.cacheIRReader.objOperandId();
    JSObject* fun = reinterpret_cast<JSObject*>(icregs.icVals[funId.id()]);
    uint16_t flags = FunctionFlags::HasJitEntryFlags();
    if (!fun->as<JSFunction>().flags().hasFlags(flags)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFunctionHasNoJitEntry) {
    ObjOperandId funId = icregs.cacheIRReader.objOperandId();
    JSObject* fun = reinterpret_cast<JSObject*>(icregs.icVals[funId.id()]);
    uint16_t flags = FunctionFlags::HasJitEntryFlags();
    if (fun->as<JSFunction>().flags().hasFlags(flags)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFunctionIsNonBuiltinCtor) {
    ObjOperandId funId = icregs.cacheIRReader.objOperandId();
    JSObject* fun = reinterpret_cast<JSObject*>(icregs.icVals[funId.id()]);
    if (!fun->as<JSFunction>().isNonBuiltinConstructor()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardFunctionIsConstructor) {
    ObjOperandId funId = icregs.cacheIRReader.objOperandId();
    JSObject* fun = reinterpret_cast<JSObject*>(icregs.icVals[funId.id()]);
    if (!fun->as<JSFunction>().isConstructor()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardNotClassConstructor) {
    ObjOperandId funId = icregs.cacheIRReader.objOperandId();
    JSObject* fun = reinterpret_cast<JSObject*>(icregs.icVals[funId.id()]);
    if (fun->as<JSFunction>().isClassConstructor()) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardArrayIsPacked) {
    ObjOperandId arrayId = icregs.cacheIRReader.objOperandId();
    JSObject* array = reinterpret_cast<JSObject*>(icregs.icVals[arrayId.id()]);
    if (!IsPackedArray(array)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(GuardArgumentsObjectFlags) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint8_t flags = icregs.cacheIRReader.readByte();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    if (obj->as<ArgumentsObject>().hasFlags(flags)) {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadObject) {
    ObjOperandId resultId = icregs.cacheIRReader.objOperandId();
    BOUNDSCHECK(resultId);
    uint32_t objOffset = icregs.cacheIRReader.stubOffset();
    intptr_t obj = cstub->stubInfo()->getStubRawWord(cstub, objOffset);
    icregs.icVals[resultId.id()] = obj;
    PREDICT_NEXT(GuardShape);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadProtoObject) {
    ObjOperandId resultId = icregs.cacheIRReader.objOperandId();
    BOUNDSCHECK(resultId);
    uint32_t protoObjOffset = icregs.cacheIRReader.stubOffset();
    ObjOperandId receiverObjId = icregs.cacheIRReader.objOperandId();
    (void)receiverObjId;
    intptr_t obj = cstub->stubInfo()->getStubRawWord(cstub, protoObjOffset);
    icregs.icVals[resultId.id()] = obj;
    PREDICT_NEXT(GuardShape);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadProto) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ObjOperandId resultId = icregs.cacheIRReader.objOperandId();
    BOUNDSCHECK(resultId);
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    icregs.icVals[resultId.id()] =
        reinterpret_cast<uintptr_t>(nobj->staticPrototype());
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadEnclosingEnvironment) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ObjOperandId resultId = icregs.cacheIRReader.objOperandId();
    BOUNDSCHECK(resultId);
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSObject* env = &obj->as<EnvironmentObject>().enclosingEnvironment();
    icregs.icVals[resultId.id()] = reinterpret_cast<uintptr_t>(env);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadWrapperTarget) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ObjOperandId resultId = icregs.cacheIRReader.objOperandId();
    bool fallible = icregs.cacheIRReader.readBool();
    BOUNDSCHECK(resultId);
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    JSObject* target = obj->as<ProxyObject>().private_().toObjectOrNull();
    if (fallible && !target) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[resultId.id()] = reinterpret_cast<uintptr_t>(target);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadValueTag) {
    ValOperandId valId = icregs.cacheIRReader.valOperandId();
    ValueTagOperandId resultId = icregs.cacheIRReader.valueTagOperandId();
    BOUNDSCHECK(resultId);
    Value val = Value::fromRawBits(icregs.icVals[valId.id()]);
    icregs.icVals[resultId.id()] = val.extractNonDoubleType();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadArgumentFixedSlot) {
    ValOperandId resultId = icregs.cacheIRReader.valOperandId();
    BOUNDSCHECK(resultId);
    uint8_t slotIndex = icregs.cacheIRReader.readByte();
    Value val = sp[slotIndex].asValue();
    TRACE_PRINTF(" -> slot %d: val %" PRIx64 "\n", int(slotIndex),
                 val.asRawBits());
    icregs.icVals[resultId.id()] = val.asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadArgumentDynamicSlot) {
    ValOperandId resultId = icregs.cacheIRReader.valOperandId();
    BOUNDSCHECK(resultId);
    Int32OperandId argcId = icregs.cacheIRReader.int32OperandId();
    uint8_t slotIndex = icregs.cacheIRReader.readByte();
    int32_t argc = int32_t(icregs.icVals[argcId.id()]);
    Value val = sp[slotIndex + argc].asValue();
    icregs.icVals[resultId.id()] = val.asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(TruncateDoubleToUInt32) {
    NumberOperandId inputId = icregs.cacheIRReader.numberOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    Value input = Value::fromRawBits(icregs.icVals[inputId.id()]);
    icregs.icVals[resultId.id()] = JS::ToInt32(input.toNumber());
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(MegamorphicLoadSlotResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t nameOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    jsid name =
        jsid::fromRawBits(cstub->stubInfo()->getStubRawWord(cstub, nameOffset));
    if (!obj->shape()->isNative()) {
      return ICInterpretOpResult::NextIC;
    }
    Value result;
    if (!GetNativeDataPropertyPureWithCacheLookup(
            frameMgr.cxForLocalUseOnly(), obj, name, nullptr, &result)) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = result.asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(MegamorphicLoadSlotByValueResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ValOperandId idId = icregs.cacheIRReader.valOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    Value id = Value::fromRawBits(icregs.icVals[idId.id()]);
    if (!obj->shape()->isNative()) {
      return ICInterpretOpResult::NextIC;
    }
    Value values[2] = {id};
    if (!GetNativeDataPropertyByValuePure(frameMgr.cxForLocalUseOnly(), obj,
                                          nullptr, values)) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = values[1].asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(MegamorphicSetElement) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ValOperandId idId = icregs.cacheIRReader.valOperandId();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    bool strict = icregs.cacheIRReader.readBool();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    Value id = Value::fromRawBits(icregs.icVals[idId.id()]);
    Value rhs = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    {
      PUSH_IC_FRAME();
      ReservedRooted<JSObject*> obj0(&state.obj0, obj);
      ReservedRooted<Value> value0(&state.value0, id);
      ReservedRooted<Value> value1(&state.value1, rhs);
      if (!SetElementMegamorphic<false>(cx, obj0, value0, value1, strict)) {
        return ICInterpretOpResult::Error;
      }
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(StoreFixedSlot) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    uintptr_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
        reinterpret_cast<uintptr_t>(nobj) + offset);
    Value val = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    slot->set(val);
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(StoreDynamicSlot) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    uint32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    HeapSlot* slots = nobj->getSlotsUnchecked();
    Value val = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    size_t dynSlot = offset / sizeof(Value);
    size_t slot = dynSlot + nobj->numFixedSlots();
    slots[dynSlot].set(nobj, HeapSlot::Slot, slot, val);
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(AddAndStoreFixedSlot) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    uint32_t newShapeOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    int32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    Value rhs = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    Shape* newShape = reinterpret_cast<Shape*>(
        cstub->stubInfo()->getStubRawWord(cstub, newShapeOffset));
    obj->setShape(newShape);
    GCPtr<Value>* slot = reinterpret_cast<GCPtr<Value>*>(
        reinterpret_cast<uintptr_t>(obj) + offset);
    slot->init(rhs);
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(AddAndStoreDynamicSlot) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    uint32_t newShapeOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    int32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    Value rhs = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    Shape* newShape = reinterpret_cast<Shape*>(
        cstub->stubInfo()->getStubRawWord(cstub, newShapeOffset));
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
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    uint32_t newShapeOffset = icregs.cacheIRReader.stubOffset();
    uint32_t numNewSlotsOffset = icregs.cacheIRReader.stubOffset();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    int32_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    Value rhs = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    Shape* newShape = reinterpret_cast<Shape*>(
        cstub->stubInfo()->getStubRawWord(cstub, newShapeOffset));
    int32_t numNewSlots =
        cstub->stubInfo()->getStubRawInt32(cstub, numNewSlotsOffset);
    NativeObject* nobj = &obj->as<NativeObject>();
    // We have to (re)allocate dynamic slots. Do this first, as it's the
    // only fallible operation here. Note that growSlotsPure is fallible but
    // does not GC. Otherwise this is the same as AddAndStoreDynamicSlot above.
    if (!NativeObject::growSlotsPure(frameMgr.cxForLocalUseOnly(), nobj,
                                     numNewSlots)) {
      return ICInterpretOpResult::NextIC;
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
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    ObjectElements* elems = nobj->getElementsHeader();
    int32_t index = int32_t(icregs.icVals[indexId.id()]);
    if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
      return ICInterpretOpResult::NextIC;
    }
    HeapSlot* slot = &elems->elements()[index];
    if (slot->get().isMagic()) {
      return ICInterpretOpResult::NextIC;
    }
    Value val = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    slot->set(nobj, HeapSlot::Element, index + elems->numShiftedElements(),
              val);
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(StoreDenseElementHole) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    bool handleAdd = icregs.cacheIRReader.readBool();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t index = uint32_t(icregs.icVals[indexId.id()]);
    Value rhs = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    NativeObject* nobj = &obj->as<NativeObject>();
    uint32_t initLength = nobj->getDenseInitializedLength();
    if (index < initLength) {
      nobj->setDenseElement(index, rhs);
    } else if (!handleAdd || index > initLength) {
      return ICInterpretOpResult::NextIC;
    } else {
      if (index >= nobj->getDenseCapacity()) {
        if (!NativeObject::addDenseElementPure(frameMgr.cxForLocalUseOnly(),
                                               nobj)) {
          return ICInterpretOpResult::NextIC;
        }
      }
      nobj->setDenseInitializedLength(initLength + 1);

      // Baseline always updates the length field by directly accessing its
      // offset in ObjectElements. If the object is not an ArrayObject then this
      // field is never read, so it's okay to skip the update here in that case.
      if (nobj->is<ArrayObject>()) {
        ArrayObject* aobj = &nobj->as<ArrayObject>();
        uint32_t len = aobj->length();
        if (len <= index) {
          aobj->setLength(len + 1);
        }
      }

      nobj->initDenseElement(index, rhs);
    }
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(ArrayPush) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ValOperandId rhsId = icregs.cacheIRReader.valOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    Value rhs = Value::fromRawBits(icregs.icVals[rhsId.id()]);
    ArrayObject* aobj = &obj->as<ArrayObject>();
    uint32_t initLength = aobj->getDenseInitializedLength();
    if (aobj->length() != initLength) {
      return ICInterpretOpResult::NextIC;
    }
    if (initLength >= aobj->getDenseCapacity()) {
      if (!NativeObject::addDenseElementPure(frameMgr.cxForLocalUseOnly(),
                                             aobj)) {
        return ICInterpretOpResult::NextIC;
      }
    }
    aobj->setDenseInitializedLength(initLength + 1);
    aobj->setLength(initLength + 1);
    aobj->initDenseElement(initLength, rhs);
    icregs.icResult = Int32Value(initLength + 1).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(IsObjectResult) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value val = Value::fromRawBits(icregs.icVals[inputId.id()]);
    icregs.icResult = BooleanValue(val.isObject()).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(Int32MinMax) {
    bool isMax = icregs.cacheIRReader.readBool();
    Int32OperandId firstId = icregs.cacheIRReader.int32OperandId();
    Int32OperandId secondId = icregs.cacheIRReader.int32OperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    int32_t lhs = int32_t(icregs.icVals[firstId.id()]);
    int32_t rhs = int32_t(icregs.icVals[secondId.id()]);
    int32_t result = ((lhs > rhs) ^ isMax) ? rhs : lhs;
    icregs.icVals[resultId.id()] = result;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(StoreTypedArrayElement) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    Scalar::Type elementType = icregs.cacheIRReader.scalarType();
    IntPtrOperandId indexId = icregs.cacheIRReader.intPtrOperandId();
    uint32_t rhsId = icregs.cacheIRReader.rawOperandId();
    bool handleOOB = icregs.cacheIRReader.readBool();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uintptr_t index = uintptr_t(icregs.icVals[indexId.id()]);
    uint64_t rhs = icregs.icVals[rhsId];
    if (obj->as<TypedArrayObject>().length().isNothing()) {
      return ICInterpretOpResult::NextIC;
    }
    if (index >= obj->as<TypedArrayObject>().length().value()) {
      if (!handleOOB) {
        return ICInterpretOpResult::NextIC;
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

      // SetTypedArrayElement doesn't do anything that can actually GC or need a
      // new context when the value can only be Int32, Double, or BigInt, as the
      // above switch statement enforces.
      FakeRooted<TypedArrayObject*> obj0(nullptr, &obj->as<TypedArrayObject>());
      FakeRooted<Value> value0(nullptr, v);
      ObjectOpResult result;
      MOZ_ASSERT(elementType == obj0->type());
      MOZ_ALWAYS_TRUE(SetTypedArrayElement(frameMgr.cxForLocalUseOnly(), obj0,
                                           index, value0, result));
      MOZ_ALWAYS_TRUE(result.ok());
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(CallInt32ToString) {
    Int32OperandId inputId = icregs.cacheIRReader.int32OperandId();
    StringOperandId resultId = icregs.cacheIRReader.stringOperandId();
    BOUNDSCHECK(resultId);
    int32_t input = int32_t(icregs.icVals[inputId.id()]);
    JSLinearString* str =
        Int32ToStringPure(frameMgr.cxForLocalUseOnly(), input);
    if (str) {
      icregs.icVals[resultId.id()] = reinterpret_cast<uintptr_t>(str);
    } else {
      return ICInterpretOpResult::NextIC;
    }
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(CallScriptedFunction)
  CACHEOP_CASE_FALLTHROUGH(CallNativeFunction) {
    bool isNative = cacheop == CacheOp::CallNativeFunction;
    TRACE_PRINTF("CallScriptedFunction / CallNativeFunction (native: %d)\n",
                 isNative);
    ObjOperandId calleeId = icregs.cacheIRReader.objOperandId();
    Int32OperandId argcId = icregs.cacheIRReader.int32OperandId();
    CallFlags flags = icregs.cacheIRReader.callFlags();
    uint32_t argcFixed = icregs.cacheIRReader.uint32Immediate();
    bool ignoresRv = false;
    if (isNative) {
      ignoresRv = icregs.cacheIRReader.readBool();
    }

    JSFunction* callee =
        reinterpret_cast<JSFunction*>(icregs.icVals[calleeId.id()]);
    uint32_t argc = uint32_t(icregs.icVals[argcId.id()]);
    (void)argcFixed;

    if (!isNative) {
      if (!callee->hasBaseScript() || !callee->baseScript()->hasBytecode() ||
          !callee->baseScript()->hasJitScript()) {
        return ICInterpretOpResult::NextIC;
      }
    }

    // For now, fail any constructing or different-realm cases.
    if (flags.isConstructing()) {
      TRACE_PRINTF("failing: constructing\n");
      return ICInterpretOpResult::NextIC;
    }
    if (!flags.isSameRealm()) {
      TRACE_PRINTF("failing: not same realm\n");
      return ICInterpretOpResult::NextIC;
    }
    // And support only "standard" arg formats.
    if (flags.getArgFormat() != CallFlags::Standard) {
      TRACE_PRINTF("failing: not standard arg format\n");
      return ICInterpretOpResult::NextIC;
    }

    // For now, fail any arg-underflow case.
    if (argc < callee->nargs()) {
      TRACE_PRINTF("failing: too few args\n");
      return ICInterpretOpResult::NextIC;
    }

    uint32_t extra = 1 + flags.isConstructing() + isNative;
    uint32_t totalArgs = argc + extra;
    StackVal* origArgs = sp;

    {
      PUSH_IC_FRAME();

      if (!stack.check(sp, sizeof(StackVal) * (totalArgs + 6))) {
        ReportOverRecursed(frameMgr.cxForLocalUseOnly());
        return ICInterpretOpResult::Error;
      }

      // This will not be an Exit frame but a BaselineStub frame, so
      // replace the ExitFrameType with the ICStub pointer.
      POPNNATIVE(1);
      PUSHNATIVE(StackValNative(cstub));

      // Push args.
      for (uint32_t i = 0; i < totalArgs; i++) {
        PUSH(origArgs[i]);
      }
      Value* args = reinterpret_cast<Value*>(sp);

      TRACE_PRINTF("pushing callee: %p\n", callee);
      PUSHNATIVE(
          StackValNative(CalleeToToken(callee, /* isConstructing = */ false)));

      if (isNative) {
        PUSHNATIVE(StackValNative(argc));
        PUSHNATIVE(StackValNative(
            MakeFrameDescriptorForJitCall(FrameType::BaselineStub, 0)));

        // We *also* need an exit frame (the native baseline
        // execution would invoke a trampoline here).
        StackVal* trampolinePrevFP = stack.fp;
        PUSHNATIVE(StackValNative(nullptr));  // fake return address.
        PUSHNATIVE(StackValNative(stack.fp));
        stack.fp = sp;
        PUSHNATIVE(StackValNative(uint32_t(ExitFrameType::CallNative)));
        cx.getCx()->activation()->asJit()->setJSExitFP(
            reinterpret_cast<uint8_t*>(stack.fp));
        cx.getCx()->portableBaselineStack().top = reinterpret_cast<void*>(sp);

        JSNative native = ignoresRv
                              ? callee->jitInfo()->ignoresReturnValueMethod
                              : callee->native();
        bool success = native(cx, argc, args);

        stack.fp = trampolinePrevFP;
        POPNNATIVE(4);

        if (!success) {
          return ICInterpretOpResult::Error;
        }
        icregs.icResult = args[0].asRawBits();
      } else {
        PUSHNATIVE(StackValNative(
            MakeFrameDescriptorForJitCall(FrameType::BaselineStub, argc)));

        switch (PortableBaselineInterpret(
            cx, state, stack, sp, /* envChain = */ nullptr,
            reinterpret_cast<Value*>(&icregs.icResult))) {
          case PBIResult::Ok:
            break;
          case PBIResult::Error:
            return ICInterpretOpResult::Error;
          case PBIResult::Unwind:
            return ICInterpretOpResult::Unwind;
          case PBIResult::UnwindError:
            return ICInterpretOpResult::UnwindError;
          case PBIResult::UnwindRet:
            return ICInterpretOpResult::UnwindRet;
        }
      }
    }

    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(MetaScriptedThisShape) {
    uint32_t thisShapeOffset = icregs.cacheIRReader.stubOffset();
    // This op is only metadata for the Warp Transpiler and should be ignored.
    (void)thisShapeOffset;
    PREDICT_NEXT(CallScriptedFunction);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadFixedSlotResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    uintptr_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    Value* slot =
        reinterpret_cast<Value*>(reinterpret_cast<uintptr_t>(nobj) + offset);
    TRACE_PRINTF(
        "LoadFixedSlotResult: obj %p offsetOffset %d offset %d slotPtr %p "
        "slot %" PRIx64 "\n",
        nobj, int(offsetOffset), int(offset), slot, slot->asRawBits());
    icregs.icResult = slot->asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadDynamicSlotResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t offsetOffset = icregs.cacheIRReader.stubOffset();
    uintptr_t offset = cstub->stubInfo()->getStubRawInt32(cstub, offsetOffset);
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    HeapSlot* slots = nobj->getSlotsUnchecked();
    icregs.icResult = slots[offset / sizeof(Value)].get().asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadDenseElementResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    NativeObject* nobj =
        reinterpret_cast<NativeObject*>(icregs.icVals[objId.id()]);
    ObjectElements* elems = nobj->getElementsHeader();
    int32_t index = int32_t(icregs.icVals[indexId.id()]);
    if (index < 0 || uint32_t(index) >= nobj->getDenseInitializedLength()) {
      return ICInterpretOpResult::NextIC;
    }
    HeapSlot* slot = &elems->elements()[index];
    Value val = slot->get();
    if (val.isMagic()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = val.asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadInt32ArrayLengthResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    ArrayObject* aobj =
        reinterpret_cast<ArrayObject*>(icregs.icVals[objId.id()]);
    uint32_t length = aobj->length();
    if (length > uint32_t(INT32_MAX)) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = Int32Value(length).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadInt32ArrayLength) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    ArrayObject* aobj =
        reinterpret_cast<ArrayObject*>(icregs.icVals[objId.id()]);
    uint32_t length = aobj->length();
    if (length > uint32_t(INT32_MAX)) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icVals[resultId.id()] = length;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadArgumentsObjectArgResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    uint32_t index = uint32_t(icregs.icVals[indexId.id()]);
    ArgumentsObject* args = &obj->as<ArgumentsObject>();
    if (index >= args->initialLength() || args->hasOverriddenElement()) {
      return ICInterpretOpResult::NextIC;
    }
    if (args->argIsForwarded(index)) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = args->arg(index).asRawBits();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LinearizeForCharAccess) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    StringOperandId resultId = icregs.cacheIRReader.stringOperandId();
    BOUNDSCHECK(resultId);
    JSString* str =
        reinterpret_cast<JSLinearString*>(icregs.icVals[strId.id()]);
    (void)indexId;

    if (!str->isRope()) {
      icregs.icVals[resultId.id()] = reinterpret_cast<uintptr_t>(str);
    } else {
      PUSH_IC_FRAME();
      JSLinearString* result = LinearizeForCharAccess(cx, str);
      if (!result) {
        return ICInterpretOpResult::Error;
      }
      icregs.icVals[resultId.id()] = reinterpret_cast<uintptr_t>(result);
    }
    PREDICT_NEXT(LoadStringCharResult);
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadStringCharResult) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    bool handleOOB = icregs.cacheIRReader.readBool();

    JSString* str =
        reinterpret_cast<JSLinearString*>(icregs.icVals[strId.id()]);
    int32_t index = int32_t(icregs.icVals[indexId.id()]);
    JSString* result = nullptr;
    if (index < 0 || size_t(index) >= str->length()) {
      if (handleOOB) {
        // Return an empty string.
        result = frameMgr.cxForLocalUseOnly()->names().empty_;
      } else {
        return ICInterpretOpResult::NextIC;
      }
    } else {
      char16_t c;
      // Guaranteed to be always work because this CacheIR op is
      // always preceded by LinearizeForCharAccess.
      MOZ_ALWAYS_TRUE(str->getChar(/* cx = */ nullptr, index, &c));
      StaticStrings& sstr = frameMgr.cxForLocalUseOnly()->staticStrings();
      if (sstr.hasUnit(c)) {
        result = sstr.getUnit(c);
      } else {
        PUSH_IC_FRAME();
        result = StringFromCharCode(cx, c);
        if (!result) {
          return ICInterpretOpResult::Error;
        }
      }
    }
    icregs.icResult = StringValue(result).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadStringCharCodeResult) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    Int32OperandId indexId = icregs.cacheIRReader.int32OperandId();
    bool handleOOB = icregs.cacheIRReader.readBool();

    JSString* str =
        reinterpret_cast<JSLinearString*>(icregs.icVals[strId.id()]);
    int32_t index = int32_t(icregs.icVals[indexId.id()]);
    Value result;
    if (index < 0 || size_t(index) >= str->length()) {
      if (handleOOB) {
        // Return NaN.
        result = JS::NaNValue();
      } else {
        return ICInterpretOpResult::NextIC;
      }
    } else {
      char16_t c;
      // Guaranteed to be always work because this CacheIR op is
      // always preceded by LinearizeForCharAccess.
      MOZ_ALWAYS_TRUE(str->getChar(/* cx = */ nullptr, index, &c));
      result = Int32Value(c);
    }
    icregs.icResult = result.asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadStringLengthResult) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    JSString* str = reinterpret_cast<JSString*>(icregs.icVals[strId.id()]);
    size_t length = str->length();
    if (length > size_t(INT32_MAX)) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = Int32Value(length).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadObjectResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    icregs.icResult =
        ObjectValue(*reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]))
            .asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadStringResult) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    icregs.icResult =
        StringValue(reinterpret_cast<JSString*>(icregs.icVals[strId.id()]))
            .asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadSymbolResult) {
    SymbolOperandId symId = icregs.cacheIRReader.symbolOperandId();
    icregs.icResult =
        SymbolValue(reinterpret_cast<JS::Symbol*>(icregs.icVals[symId.id()]))
            .asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadInt32Result) {
    Int32OperandId valId = icregs.cacheIRReader.int32OperandId();
    icregs.icResult = Int32Value(icregs.icVals[valId.id()]).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadDoubleResult) {
    NumberOperandId valId = icregs.cacheIRReader.numberOperandId();
    Value val = Value::fromRawBits(icregs.icVals[valId.id()]);
    if (val.isInt32()) {
      val = DoubleValue(val.toInt32());
    }
    icregs.icResult = val.asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadBigIntResult) {
    BigIntOperandId valId = icregs.cacheIRReader.bigIntOperandId();
    icregs.icResult =
        BigIntValue(reinterpret_cast<JS::BigInt*>(icregs.icVals[valId.id()]))
            .asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadBooleanResult) {
    bool val = icregs.cacheIRReader.readBool();
    icregs.icResult = BooleanValue(val).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadInt32Constant) {
    uint32_t valOffset = icregs.cacheIRReader.stubOffset();
    Int32OperandId resultId = icregs.cacheIRReader.int32OperandId();
    BOUNDSCHECK(resultId);
    uint32_t value = cstub->stubInfo()->getStubRawInt32(cstub, valOffset);
    icregs.icVals[resultId.id()] = value;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadConstantStringResult) {
    uint32_t strOffset = icregs.cacheIRReader.stubOffset();
    JSString* str = reinterpret_cast<JSString*>(
        cstub->stubInfo()->getStubRawWord(cstub, strOffset));
    icregs.icResult = StringValue(str).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

#define INT32_OP(name, op, extra_check)                           \
  CACHEOP_CASE(Int32##name##Result) {                             \
    Int32OperandId lhsId = icregs.cacheIRReader.int32OperandId(); \
    Int32OperandId rhsId = icregs.cacheIRReader.int32OperandId(); \
    int64_t lhs = int64_t(int32_t(icregs.icVals[lhsId.id()]));    \
    int64_t rhs = int64_t(int32_t(icregs.icVals[rhsId.id()]));    \
    extra_check;                                                  \
    int64_t result = lhs op rhs;                                  \
    if (result < INT32_MIN || result > INT32_MAX) {               \
      return ICInterpretOpResult::NextIC;                         \
    }                                                             \
    icregs.icResult = Int32Value(int32_t(result)).asRawBits();    \
    PREDICT_RETURN();                                             \
    DISPATCH_CACHEOP();                                           \
  }

  // clang-format off
  INT32_OP(Add, +, {});
  INT32_OP(Sub, -, {});
  // clang-format on
  INT32_OP(Mul, *, {
    if (rhs * lhs == 0 && ((rhs < 0) ^ (lhs < 0))) {
      return ICInterpretOpResult::NextIC;
    }
  });
  INT32_OP(Div, /, {
    if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) {
      return ICInterpretOpResult::NextIC;
    }
    if (lhs == 0 && rhs < 0) {
      return ICInterpretOpResult::NextIC;
    }
    if (lhs % rhs != 0) {
      return ICInterpretOpResult::NextIC;
    }
  });
  INT32_OP(Mod, %, {
    if (rhs == 0 || (lhs == INT32_MIN && rhs == -1)) {
      return ICInterpretOpResult::NextIC;
    }
    if (lhs % rhs == 0 && lhs < 0) {
      return ICInterpretOpResult::NextIC;
    }
  });
  // clang-format off
  INT32_OP(BitOr, |, {});
  INT32_OP(BitXor, ^, {});
  INT32_OP(BitAnd, &, {});
  // clang-format on

  CACHEOP_CASE(Int32PowResult) {
    Int32OperandId lhsId = icregs.cacheIRReader.int32OperandId();
    Int32OperandId rhsId = icregs.cacheIRReader.int32OperandId();
    int64_t lhs = int64_t(int32_t(icregs.icVals[lhsId.id()]));
    int64_t rhs = int64_t(int32_t(icregs.icVals[rhsId.id()]));
    int64_t result;

    if (lhs == 1) {
      result = 1;
    } else if (rhs < 0) {
      return ICInterpretOpResult::NextIC;
    } else {
      result = 1;
      int64_t runningSquare = lhs;
      while (rhs) {
        if (rhs & 1) {
          result *= runningSquare;
          if (result > int64_t(INT32_MAX)) {
            return ICInterpretOpResult::NextIC;
          }
        }
        rhs >>= 1;
        if (rhs == 0) {
          break;
        }
        runningSquare *= runningSquare;
        if (runningSquare > int64_t(INT32_MAX)) {
          return ICInterpretOpResult::NextIC;
        }
      }
    }

    icregs.icResult = Int32Value(int32_t(result)).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(Int32IncResult) {
    Int32OperandId inputId = icregs.cacheIRReader.int32OperandId();
    int64_t value = int64_t(int32_t(icregs.icVals[inputId.id()]));
    value++;
    if (value > INT32_MAX) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = Int32Value(int32_t(value)).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadInt32TruthyResult) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    int32_t val = int32_t(icregs.icVals[inputId.id()]);
    icregs.icResult = BooleanValue(val != 0).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadStringTruthyResult) {
    StringOperandId strId = icregs.cacheIRReader.stringOperandId();
    JSString* str =
        reinterpret_cast<JSLinearString*>(icregs.icVals[strId.id()]);
    icregs.icResult = BooleanValue(str->length() > 0).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadObjectTruthyResult) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    JSObject* obj = reinterpret_cast<JSObject*>(icregs.icVals[objId.id()]);
    const JSClass* cls = obj->getClass();
    if (cls->isProxyObject()) {
      return ICInterpretOpResult::NextIC;
    }
    icregs.icResult = BooleanValue(!cls->emulatesUndefined()).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadValueResult) {
    uint32_t valOffset = icregs.cacheIRReader.stubOffset();
    icregs.icResult = cstub->stubInfo()->getStubRawInt64(cstub, valOffset);
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(LoadOperandResult) {
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    icregs.icResult = icregs.icVals[inputId.id()];
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(CallStringConcatResult) {
    StringOperandId lhsId = icregs.cacheIRReader.stringOperandId();
    StringOperandId rhsId = icregs.cacheIRReader.stringOperandId();
    // We don't push a frame and do a CanGC invocation here; we do a
    // pure (NoGC) invocation only, because it's cheaper.
    FakeRooted<JSString*> lhs(
        nullptr, reinterpret_cast<JSString*>(icregs.icVals[lhsId.id()]));
    FakeRooted<JSString*> rhs(
        nullptr, reinterpret_cast<JSString*>(icregs.icVals[rhsId.id()]));
    JSString* result =
        ConcatStrings<NoGC>(frameMgr.cxForLocalUseOnly(), lhs, rhs);
    if (result) {
      icregs.icResult = StringValue(result).asRawBits();
    } else {
      return ICInterpretOpResult::NextIC;
    }
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(CompareStringResult) {
    JSOp op = icregs.cacheIRReader.jsop();
    StringOperandId lhsId = icregs.cacheIRReader.stringOperandId();
    StringOperandId rhsId = icregs.cacheIRReader.stringOperandId();
    {
      PUSH_IC_FRAME();
      ReservedRooted<JSString*> lhs(
          &state.str0, reinterpret_cast<JSString*>(icregs.icVals[lhsId.id()]));
      ReservedRooted<JSString*> rhs(
          &state.str1, reinterpret_cast<JSString*>(icregs.icVals[rhsId.id()]));
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
              return ICInterpretOpResult::Error;
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
            if (!StringsEqual<EqualityKind::NotEqual>(cx, lhs, rhs, &result)) {
              return ICInterpretOpResult::Error;
            }
            break;
          case JSOp::Lt:
            if (!StringsCompare<ComparisonKind::LessThan>(cx, lhs, rhs,
                                                          &result)) {
              return ICInterpretOpResult::Error;
            }
            break;
          case JSOp::Ge:
            if (!StringsCompare<ComparisonKind::GreaterThanOrEqual>(
                    cx, lhs, rhs, &result)) {
              return ICInterpretOpResult::Error;
            }
            break;
          case JSOp::Le:
            if (!StringsCompare<ComparisonKind::GreaterThanOrEqual>(
                    cx, /* N.B. swapped order */ rhs, lhs, &result)) {
              return ICInterpretOpResult::Error;
            }
            break;
          case JSOp::Gt:
            if (!StringsCompare<ComparisonKind::LessThan>(
                    cx, /* N.B. swapped order */ rhs, lhs, &result)) {
              return ICInterpretOpResult::Error;
            }
            break;
          default:
            MOZ_CRASH("bad opcode");
        }
      }
      icregs.icResult = BooleanValue(result).asRawBits();
    }
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(CompareInt32Result) {
    JSOp op = icregs.cacheIRReader.jsop();
    Int32OperandId lhsId = icregs.cacheIRReader.int32OperandId();
    Int32OperandId rhsId = icregs.cacheIRReader.int32OperandId();
    int64_t lhs = int64_t(int32_t(icregs.icVals[lhsId.id()]));
    int64_t rhs = int64_t(int32_t(icregs.icVals[rhsId.id()]));
    TRACE_PRINTF("lhs (%d) = %" PRIi64 " rhs (%d) = %" PRIi64 "\n", lhsId.id(),
                 lhs, rhsId.id(), rhs);
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
    icregs.icResult = BooleanValue(result).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(CompareNullUndefinedResult) {
    JSOp op = icregs.cacheIRReader.jsop();
    bool isUndefined = icregs.cacheIRReader.readBool();
    ValOperandId inputId = icregs.cacheIRReader.valOperandId();
    Value val = Value::fromRawBits(icregs.icVals[inputId.id()]);
    if (val.isObject() && val.toObject().getClass()->isProxyObject()) {
      return ICInterpretOpResult::NextIC;
    }

    bool result;
    switch (op) {
      case JSOp::Eq:
        result =
            val.isUndefined() || val.isNull() ||
            (val.isObject() && val.toObject().getClass()->emulatesUndefined());
        break;
      case JSOp::Ne:
        result = !(
            val.isUndefined() || val.isNull() ||
            (val.isObject() && val.toObject().getClass()->emulatesUndefined()));
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
    icregs.icResult = BooleanValue(result).asRawBits();
    PREDICT_RETURN();
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE(AssertPropertyLookup) {
    ObjOperandId objId = icregs.cacheIRReader.objOperandId();
    uint32_t idOffset = icregs.cacheIRReader.stubOffset();
    uint32_t slotOffset = icregs.cacheIRReader.stubOffset();
    // Debug-only assertion; we can ignore.
    (void)objId;
    (void)idOffset;
    (void)slotOffset;
    DISPATCH_CACHEOP();
  }

  CACHEOP_CASE_UNIMPL(GuardNumberToIntPtrIndex)
  CACHEOP_CASE_UNIMPL(GuardToUint8Clamped)
  CACHEOP_CASE_UNIMPL(GuardMultipleShapes)
  CACHEOP_CASE_UNIMPL(CallRegExpMatcherResult)
  CACHEOP_CASE_UNIMPL(CallRegExpSearcherResult)
  CACHEOP_CASE_UNIMPL(RegExpSearcherLastLimitResult)
  CACHEOP_CASE_UNIMPL(RegExpHasCaptureGroupsResult)
  CACHEOP_CASE_UNIMPL(RegExpBuiltinExecMatchResult)
  CACHEOP_CASE_UNIMPL(RegExpBuiltinExecTestResult)
  CACHEOP_CASE_UNIMPL(RegExpFlagResult)
  CACHEOP_CASE_UNIMPL(CallSubstringKernelResult)
  CACHEOP_CASE_UNIMPL(StringReplaceStringResult)
  CACHEOP_CASE_UNIMPL(StringSplitStringResult)
  CACHEOP_CASE_UNIMPL(RegExpPrototypeOptimizableResult)
  CACHEOP_CASE_UNIMPL(RegExpInstanceOptimizableResult)
  CACHEOP_CASE_UNIMPL(GetFirstDollarIndexResult)
  CACHEOP_CASE_UNIMPL(GuardIsFixedLengthTypedArray)
  CACHEOP_CASE_UNIMPL(GuardIsResizableTypedArray)
  CACHEOP_CASE_UNIMPL(StringToAtom)
  CACHEOP_CASE_UNIMPL(GuardIndexIsValidUpdateOrAdd)
  CACHEOP_CASE_UNIMPL(GuardIndexIsNotDenseElement)
  CACHEOP_CASE_UNIMPL(GuardTagNotEqual)
  CACHEOP_CASE_UNIMPL(GuardXrayExpandoShapeAndDefaultProto)
  CACHEOP_CASE_UNIMPL(GuardXrayNoExpando)
  CACHEOP_CASE_UNIMPL(GuardEitherClass)
  CACHEOP_CASE_UNIMPL(LoadScriptedProxyHandler)
  CACHEOP_CASE_UNIMPL(IdToStringOrSymbol)
  CACHEOP_CASE_UNIMPL(DoubleToUint8Clamped)
  CACHEOP_CASE_UNIMPL(MegamorphicStoreSlot)
  CACHEOP_CASE_UNIMPL(MegamorphicHasPropResult)
  CACHEOP_CASE_UNIMPL(SmallObjectVariableKeyHasOwnResult)
  CACHEOP_CASE_UNIMPL(ObjectToIteratorResult)
  CACHEOP_CASE_UNIMPL(ValueToIteratorResult)
  CACHEOP_CASE_UNIMPL(LoadDOMExpandoValue)
  CACHEOP_CASE_UNIMPL(LoadDOMExpandoValueGuardGeneration)
  CACHEOP_CASE_UNIMPL(LoadDOMExpandoValueIgnoreGeneration)
  CACHEOP_CASE_UNIMPL(GuardDOMExpandoMissingOrGuardShape)
  CACHEOP_CASE_UNIMPL(AddSlotAndCallAddPropHook)
  CACHEOP_CASE_UNIMPL(ArrayJoinResult)
  CACHEOP_CASE_UNIMPL(ObjectKeysResult)
  CACHEOP_CASE_UNIMPL(PackedArrayPopResult)
  CACHEOP_CASE_UNIMPL(PackedArrayShiftResult)
  CACHEOP_CASE_UNIMPL(PackedArraySliceResult)
  CACHEOP_CASE_UNIMPL(ArgumentsSliceResult)
  CACHEOP_CASE_UNIMPL(IsArrayResult)
  CACHEOP_CASE_UNIMPL(StoreFixedSlotUndefinedResult)
  CACHEOP_CASE_UNIMPL(IsPackedArrayResult)
  CACHEOP_CASE_UNIMPL(IsCallableResult)
  CACHEOP_CASE_UNIMPL(IsConstructorResult)
  CACHEOP_CASE_UNIMPL(IsCrossRealmArrayConstructorResult)
  CACHEOP_CASE_UNIMPL(IsTypedArrayResult)
  CACHEOP_CASE_UNIMPL(IsTypedArrayConstructorResult)
  CACHEOP_CASE_UNIMPL(ArrayBufferViewByteOffsetInt32Result)
  CACHEOP_CASE_UNIMPL(ArrayBufferViewByteOffsetDoubleResult)
  CACHEOP_CASE_UNIMPL(ResizableTypedArrayByteOffsetMaybeOutOfBoundsInt32Result)
  CACHEOP_CASE_UNIMPL(ResizableTypedArrayByteOffsetMaybeOutOfBoundsDoubleResult)
  CACHEOP_CASE_UNIMPL(TypedArrayByteLengthInt32Result)
  CACHEOP_CASE_UNIMPL(TypedArrayByteLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(ResizableTypedArrayByteLengthInt32Result)
  CACHEOP_CASE_UNIMPL(ResizableTypedArrayByteLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(ResizableTypedArrayLengthInt32Result)
  CACHEOP_CASE_UNIMPL(ResizableTypedArrayLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(TypedArrayElementSizeResult)
  CACHEOP_CASE_UNIMPL(ResizableDataViewByteLengthInt32Result)
  CACHEOP_CASE_UNIMPL(ResizableDataViewByteLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(GrowableSharedArrayBufferByteLengthInt32Result)
  CACHEOP_CASE_UNIMPL(GrowableSharedArrayBufferByteLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(GuardHasAttachedArrayBuffer)
  CACHEOP_CASE_UNIMPL(GuardResizableArrayBufferViewInBounds)
  CACHEOP_CASE_UNIMPL(GuardResizableArrayBufferViewInBoundsOrDetached)
  CACHEOP_CASE_UNIMPL(NewArrayIteratorResult)
  CACHEOP_CASE_UNIMPL(NewStringIteratorResult)
  CACHEOP_CASE_UNIMPL(NewRegExpStringIteratorResult)
  CACHEOP_CASE_UNIMPL(ObjectCreateResult)
  CACHEOP_CASE_UNIMPL(NewArrayFromLengthResult)
  CACHEOP_CASE_UNIMPL(NewTypedArrayFromLengthResult)
  CACHEOP_CASE_UNIMPL(NewTypedArrayFromArrayBufferResult)
  CACHEOP_CASE_UNIMPL(NewTypedArrayFromArrayResult)
  CACHEOP_CASE_UNIMPL(NewStringObjectResult)
  CACHEOP_CASE_UNIMPL(StringFromCharCodeResult)
  CACHEOP_CASE_UNIMPL(StringFromCodePointResult)
  CACHEOP_CASE_UNIMPL(StringIncludesResult)
  CACHEOP_CASE_UNIMPL(StringIndexOfResult)
  CACHEOP_CASE_UNIMPL(StringLastIndexOfResult)
  CACHEOP_CASE_UNIMPL(StringStartsWithResult)
  CACHEOP_CASE_UNIMPL(StringEndsWithResult)
  CACHEOP_CASE_UNIMPL(StringToLowerCaseResult)
  CACHEOP_CASE_UNIMPL(StringToUpperCaseResult)
  CACHEOP_CASE_UNIMPL(StringTrimResult)
  CACHEOP_CASE_UNIMPL(StringTrimStartResult)
  CACHEOP_CASE_UNIMPL(StringTrimEndResult)
  CACHEOP_CASE_UNIMPL(LinearizeForCodePointAccess)
  CACHEOP_CASE_UNIMPL(LoadStringAtResult)
  CACHEOP_CASE_UNIMPL(LoadStringCodePointResult)
  CACHEOP_CASE_UNIMPL(ToRelativeStringIndex)
  CACHEOP_CASE_UNIMPL(MathAbsInt32Result)
  CACHEOP_CASE_UNIMPL(MathAbsNumberResult)
  CACHEOP_CASE_UNIMPL(MathClz32Result)
  CACHEOP_CASE_UNIMPL(MathSignInt32Result)
  CACHEOP_CASE_UNIMPL(MathSignNumberResult)
  CACHEOP_CASE_UNIMPL(MathSignNumberToInt32Result)
  CACHEOP_CASE_UNIMPL(MathImulResult)
  CACHEOP_CASE_UNIMPL(MathSqrtNumberResult)
  CACHEOP_CASE_UNIMPL(MathFRoundNumberResult)
  CACHEOP_CASE_UNIMPL(MathRandomResult)
  CACHEOP_CASE_UNIMPL(MathHypot2NumberResult)
  CACHEOP_CASE_UNIMPL(MathHypot3NumberResult)
  CACHEOP_CASE_UNIMPL(MathHypot4NumberResult)
  CACHEOP_CASE_UNIMPL(MathAtan2NumberResult)
  CACHEOP_CASE_UNIMPL(MathFloorNumberResult)
  CACHEOP_CASE_UNIMPL(MathCeilNumberResult)
  CACHEOP_CASE_UNIMPL(MathTruncNumberResult)
  CACHEOP_CASE_UNIMPL(MathFloorToInt32Result)
  CACHEOP_CASE_UNIMPL(MathCeilToInt32Result)
  CACHEOP_CASE_UNIMPL(MathTruncToInt32Result)
  CACHEOP_CASE_UNIMPL(MathRoundToInt32Result)
  CACHEOP_CASE_UNIMPL(NumberMinMax)
  CACHEOP_CASE_UNIMPL(Int32MinMaxArrayResult)
  CACHEOP_CASE_UNIMPL(NumberMinMaxArrayResult)
  CACHEOP_CASE_UNIMPL(MathFunctionNumberResult)
  CACHEOP_CASE_UNIMPL(NumberParseIntResult)
  CACHEOP_CASE_UNIMPL(DoubleParseIntResult)
  CACHEOP_CASE_UNIMPL(ObjectToStringResult)
  CACHEOP_CASE_UNIMPL(ReflectGetPrototypeOfResult)
  CACHEOP_CASE_UNIMPL(AtomicsCompareExchangeResult)
  CACHEOP_CASE_UNIMPL(AtomicsExchangeResult)
  CACHEOP_CASE_UNIMPL(AtomicsAddResult)
  CACHEOP_CASE_UNIMPL(AtomicsSubResult)
  CACHEOP_CASE_UNIMPL(AtomicsAndResult)
  CACHEOP_CASE_UNIMPL(AtomicsOrResult)
  CACHEOP_CASE_UNIMPL(AtomicsXorResult)
  CACHEOP_CASE_UNIMPL(AtomicsLoadResult)
  CACHEOP_CASE_UNIMPL(AtomicsStoreResult)
  CACHEOP_CASE_UNIMPL(AtomicsIsLockFreeResult)
  CACHEOP_CASE_UNIMPL(CallNativeSetter)
  CACHEOP_CASE_UNIMPL(CallScriptedSetter)
  CACHEOP_CASE_UNIMPL(CallInlinedSetter)
  CACHEOP_CASE_UNIMPL(CallDOMSetter)
  CACHEOP_CASE_UNIMPL(CallSetArrayLength)
  CACHEOP_CASE_UNIMPL(ProxySet)
  CACHEOP_CASE_UNIMPL(ProxySetByValue)
  CACHEOP_CASE_UNIMPL(CallAddOrUpdateSparseElementHelper)
  CACHEOP_CASE_UNIMPL(CallNumberToString)
  CACHEOP_CASE_UNIMPL(Int32ToStringWithBaseResult)
  CACHEOP_CASE_UNIMPL(BooleanToString)
  CACHEOP_CASE_UNIMPL(CallBoundScriptedFunction)
  CACHEOP_CASE_UNIMPL(CallWasmFunction)
  CACHEOP_CASE_UNIMPL(GuardWasmArg)
  CACHEOP_CASE_UNIMPL(CallDOMFunction)
  CACHEOP_CASE_UNIMPL(CallClassHook)
  CACHEOP_CASE_UNIMPL(CallInlinedFunction)
#ifdef JS_PUNBOX64
  CACHEOP_CASE_UNIMPL(CallScriptedProxyGetResult)
  CACHEOP_CASE_UNIMPL(CallScriptedProxyGetByValueResult)
#endif
  CACHEOP_CASE_UNIMPL(BindFunctionResult)
  CACHEOP_CASE_UNIMPL(SpecializedBindFunctionResult)
  CACHEOP_CASE_UNIMPL(LoadFixedSlotTypedResult)
  CACHEOP_CASE_UNIMPL(LoadDenseElementHoleResult)
  CACHEOP_CASE_UNIMPL(CallGetSparseElementResult)
  CACHEOP_CASE_UNIMPL(LoadDenseElementExistsResult)
  CACHEOP_CASE_UNIMPL(LoadTypedArrayElementExistsResult)
  CACHEOP_CASE_UNIMPL(LoadDenseElementHoleExistsResult)
  CACHEOP_CASE_UNIMPL(LoadTypedArrayElementResult)
  CACHEOP_CASE_UNIMPL(LoadDataViewValueResult)
  CACHEOP_CASE_UNIMPL(StoreDataViewValueResult)
  CACHEOP_CASE_UNIMPL(LoadArgumentsObjectArgHoleResult)
  CACHEOP_CASE_UNIMPL(LoadArgumentsObjectArgExistsResult)
  CACHEOP_CASE_UNIMPL(LoadArgumentsObjectLengthResult)
  CACHEOP_CASE_UNIMPL(LoadArgumentsObjectLength)
  CACHEOP_CASE_UNIMPL(LoadFunctionLengthResult)
  CACHEOP_CASE_UNIMPL(LoadFunctionNameResult)
  CACHEOP_CASE_UNIMPL(LoadBoundFunctionNumArgs)
  CACHEOP_CASE_UNIMPL(LoadBoundFunctionTarget)
  CACHEOP_CASE_UNIMPL(GuardBoundFunctionIsConstructor)
  CACHEOP_CASE_UNIMPL(LoadArrayBufferByteLengthInt32Result)
  CACHEOP_CASE_UNIMPL(LoadArrayBufferByteLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(LoadArrayBufferViewLengthInt32Result)
  CACHEOP_CASE_UNIMPL(LoadArrayBufferViewLengthDoubleResult)
  CACHEOP_CASE_UNIMPL(FrameIsConstructingResult)
  CACHEOP_CASE_UNIMPL(CallScriptedGetterResult)
  CACHEOP_CASE_UNIMPL(CallInlinedGetterResult)
  CACHEOP_CASE_UNIMPL(CallNativeGetterResult)
  CACHEOP_CASE_UNIMPL(CallDOMGetterResult)
  CACHEOP_CASE_UNIMPL(ProxyGetResult)
  CACHEOP_CASE_UNIMPL(ProxyGetByValueResult)
  CACHEOP_CASE_UNIMPL(ProxyHasPropResult)
  CACHEOP_CASE_UNIMPL(CallObjectHasSparseElementResult)
  CACHEOP_CASE_UNIMPL(CallNativeGetElementResult)
  CACHEOP_CASE_UNIMPL(CallNativeGetElementSuperResult)
  CACHEOP_CASE_UNIMPL(GetNextMapSetEntryForIteratorResult)
  CACHEOP_CASE_UNIMPL(LoadUndefinedResult)
  CACHEOP_CASE_UNIMPL(LoadDoubleConstant)
  CACHEOP_CASE_UNIMPL(LoadBooleanConstant)
  CACHEOP_CASE_UNIMPL(LoadUndefined)
  CACHEOP_CASE_UNIMPL(LoadConstantString)
  CACHEOP_CASE_UNIMPL(LoadInstanceOfObjectResult)
  CACHEOP_CASE_UNIMPL(LoadTypeOfObjectResult)
  CACHEOP_CASE_UNIMPL(LoadTypeOfEqObjectResult)
  CACHEOP_CASE_UNIMPL(DoubleAddResult)
  CACHEOP_CASE_UNIMPL(DoubleSubResult)
  CACHEOP_CASE_UNIMPL(DoubleMulResult)
  CACHEOP_CASE_UNIMPL(DoubleDivResult)
  CACHEOP_CASE_UNIMPL(DoubleModResult)
  CACHEOP_CASE_UNIMPL(DoublePowResult)
  CACHEOP_CASE_UNIMPL(BigIntAddResult)
  CACHEOP_CASE_UNIMPL(BigIntSubResult)
  CACHEOP_CASE_UNIMPL(BigIntMulResult)
  CACHEOP_CASE_UNIMPL(BigIntDivResult)
  CACHEOP_CASE_UNIMPL(BigIntModResult)
  CACHEOP_CASE_UNIMPL(BigIntPowResult)
  CACHEOP_CASE_UNIMPL(Int32LeftShiftResult)
  CACHEOP_CASE_UNIMPL(Int32RightShiftResult)
  CACHEOP_CASE_UNIMPL(Int32URightShiftResult)
  CACHEOP_CASE_UNIMPL(Int32NotResult)
  CACHEOP_CASE_UNIMPL(BigIntBitOrResult)
  CACHEOP_CASE_UNIMPL(BigIntBitXorResult)
  CACHEOP_CASE_UNIMPL(BigIntBitAndResult)
  CACHEOP_CASE_UNIMPL(BigIntLeftShiftResult)
  CACHEOP_CASE_UNIMPL(BigIntRightShiftResult)
  CACHEOP_CASE_UNIMPL(BigIntNotResult)
  CACHEOP_CASE_UNIMPL(Int32NegationResult)
  CACHEOP_CASE_UNIMPL(DoubleNegationResult)
  CACHEOP_CASE_UNIMPL(BigIntNegationResult)
  CACHEOP_CASE_UNIMPL(Int32DecResult)
  CACHEOP_CASE_UNIMPL(DoubleIncResult)
  CACHEOP_CASE_UNIMPL(DoubleDecResult)
  CACHEOP_CASE_UNIMPL(BigIntIncResult)
  CACHEOP_CASE_UNIMPL(BigIntDecResult)
  CACHEOP_CASE_UNIMPL(LoadDoubleTruthyResult)
  CACHEOP_CASE_UNIMPL(LoadBigIntTruthyResult)
  CACHEOP_CASE_UNIMPL(LoadValueTruthyResult)
  CACHEOP_CASE_UNIMPL(NewPlainObjectResult)
  CACHEOP_CASE_UNIMPL(NewArrayObjectResult)
  CACHEOP_CASE_UNIMPL(CallStringObjectConcatResult)
  CACHEOP_CASE_UNIMPL(CallIsSuspendedGeneratorResult)
  CACHEOP_CASE_UNIMPL(CompareObjectResult)
  CACHEOP_CASE_UNIMPL(CompareSymbolResult)
  CACHEOP_CASE_UNIMPL(CompareDoubleResult)
  CACHEOP_CASE_UNIMPL(CompareBigIntResult)
  CACHEOP_CASE_UNIMPL(CompareBigIntInt32Result)
  CACHEOP_CASE_UNIMPL(CompareBigIntNumberResult)
  CACHEOP_CASE_UNIMPL(CompareBigIntStringResult)
  CACHEOP_CASE_UNIMPL(CompareDoubleSameValueResult)
  CACHEOP_CASE_UNIMPL(SameValueResult)
  CACHEOP_CASE_UNIMPL(IndirectTruncateInt32Result)
  CACHEOP_CASE_UNIMPL(BigIntAsIntNResult)
  CACHEOP_CASE_UNIMPL(BigIntAsUintNResult)
  CACHEOP_CASE_UNIMPL(SetHasResult)
  CACHEOP_CASE_UNIMPL(SetHasNonGCThingResult)
  CACHEOP_CASE_UNIMPL(SetHasStringResult)
  CACHEOP_CASE_UNIMPL(SetHasSymbolResult)
  CACHEOP_CASE_UNIMPL(SetHasBigIntResult)
  CACHEOP_CASE_UNIMPL(SetHasObjectResult)
  CACHEOP_CASE_UNIMPL(SetSizeResult)
  CACHEOP_CASE_UNIMPL(MapHasResult)
  CACHEOP_CASE_UNIMPL(MapHasNonGCThingResult)
  CACHEOP_CASE_UNIMPL(MapHasStringResult)
  CACHEOP_CASE_UNIMPL(MapHasSymbolResult)
  CACHEOP_CASE_UNIMPL(MapHasBigIntResult)
  CACHEOP_CASE_UNIMPL(MapHasObjectResult)
  CACHEOP_CASE_UNIMPL(MapGetResult)
  CACHEOP_CASE_UNIMPL(MapGetNonGCThingResult)
  CACHEOP_CASE_UNIMPL(MapGetStringResult)
  CACHEOP_CASE_UNIMPL(MapGetSymbolResult)
  CACHEOP_CASE_UNIMPL(MapGetBigIntResult)
  CACHEOP_CASE_UNIMPL(MapGetObjectResult)
  CACHEOP_CASE_UNIMPL(MapSizeResult)
  CACHEOP_CASE_UNIMPL(ArrayFromArgumentsObjectResult)
  CACHEOP_CASE_UNIMPL(CloseIterScriptedResult)
  CACHEOP_CASE_UNIMPL(CallPrintString)
  CACHEOP_CASE_UNIMPL(Breakpoint)
  CACHEOP_CASE_UNIMPL(WrapResult)
  CACHEOP_CASE_UNIMPL(Bailout)
  CACHEOP_CASE_UNIMPL(AssertRecoveredOnBailoutResult) {
    TRACE_PRINTF("unknown CacheOp: %s\n", CacheIROpNames[int(cacheop)]);
    return ICInterpretOpResult::NextIC;
  }

#undef PREDICT_NEXT
}

/*
 * -----------------------------------------------
 * IC callsite logic, and fallback stubs
 * -----------------------------------------------
 */

#define SAVE_INPUTS(arity)            \
  do {                                \
    switch (arity) {                  \
      case 0:                         \
        break;                        \
      case 1:                         \
        inputs[0] = icregs.icVals[0]; \
        break;                        \
      case 2:                         \
        inputs[0] = icregs.icVals[0]; \
        inputs[1] = icregs.icVals[1]; \
        break;                        \
      case 3:                         \
        inputs[0] = icregs.icVals[0]; \
        inputs[1] = icregs.icVals[1]; \
        inputs[2] = icregs.icVals[2]; \
        break;                        \
    }                                 \
  } while (0)

#define RESTORE_INPUTS(arity)         \
  do {                                \
    switch (arity) {                  \
      case 0:                         \
        break;                        \
      case 1:                         \
        icregs.icVals[0] = inputs[0]; \
        break;                        \
      case 2:                         \
        icregs.icVals[0] = inputs[0]; \
        icregs.icVals[1] = inputs[1]; \
        break;                        \
      case 3:                         \
        icregs.icVals[0] = inputs[0]; \
        icregs.icVals[1] = inputs[1]; \
        icregs.icVals[2] = inputs[2]; \
        break;                        \
    }                                 \
  } while (0)

#define DEFINE_IC(kind, arity, fallback_body)                             \
  static PBIResult MOZ_ALWAYS_INLINE IC##kind(                            \
      BaselineFrame* frame, VMFrameManager& frameMgr, State& state,       \
      ICRegs& icregs, Stack& stack, StackVal* sp, jsbytecode* pc) {       \
    ICStub* stub = frame->interpreterICEntry()->firstStub();              \
    uint64_t inputs[3];                                                   \
    SAVE_INPUTS(arity);                                                   \
    while (true) {                                                        \
    next_stub:                                                            \
      if (stub->isFallback()) {                                           \
        ICFallbackStub* fallback = stub->toFallbackStub();                \
        fallback_body;                                                    \
        icregs.icResult = state.res.asRawBits();                          \
        state.res = UndefinedValue();                                     \
        return PBIResult::Ok;                                             \
      error:                                                              \
        return PBIResult::Error;                                          \
      } else {                                                            \
        ICCacheIRStub* cstub = stub->toCacheIRStub();                     \
        cstub->incrementEnteredCount();                                   \
        new (&icregs.cacheIRReader) CacheIRReader(cstub->stubInfo());     \
        switch (ICInterpretOps(frame, frameMgr, state, icregs, stack, sp, \
                               cstub, pc)) {                              \
          case ICInterpretOpResult::NextIC:                               \
            stub = stub->maybeNext();                                     \
            RESTORE_INPUTS(arity);                                        \
            goto next_stub;                                               \
          case ICInterpretOpResult::Return:                               \
            return PBIResult::Ok;                                         \
          case ICInterpretOpResult::Error:                                \
            return PBIResult::Error;                                      \
          case ICInterpretOpResult::Unwind:                               \
            return PBIResult::Unwind;                                     \
          case ICInterpretOpResult::UnwindError:                          \
            return PBIResult::UnwindError;                                \
          case ICInterpretOpResult::UnwindRet:                            \
            return PBIResult::UnwindRet;                                  \
        }                                                                 \
      }                                                                   \
    }                                                                     \
  }

#define IC_LOAD_VAL(state_elem, index)                \
  ReservedRooted<Value> state_elem(&state.state_elem, \
                                   Value::fromRawBits(icregs.icVals[(index)]))
#define IC_LOAD_OBJ(state_elem, index)  \
  ReservedRooted<JSObject*> state_elem( \
      &state.state_elem, reinterpret_cast<JSObject*>(icregs.icVals[(index)]))

DEFINE_IC(Typeof, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoTypeOfFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(TypeofEq, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoTypeOfEqFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetName, 1, {
  IC_LOAD_OBJ(obj0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetNameFallback(cx, frame, fallback, obj0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(Call, 1, {
  uint32_t argc = uint32_t(icregs.icVals[0]);
  uint32_t totalArgs =
      argc + icregs.extraArgs;  // this, callee, (constructing?), func args
  Value* args = reinterpret_cast<Value*>(&sp[0]);
  TRACE_PRINTF("Call fallback: argc %d totalArgs %d args %p\n", argc, totalArgs,
               args);
  // Reverse values on the stack.
  std::reverse(args, args + totalArgs);
  {
    PUSH_FALLBACK_IC_FRAME();
    if (icregs.spreadCall) {
      if (!DoSpreadCallFallback(cx, frame, fallback, args, &state.res)) {
        std::reverse(args, args + totalArgs);
        goto error;
      }
    } else {
      if (!DoCallFallback(cx, frame, fallback, argc, args, &state.res)) {
        std::reverse(args, args + totalArgs);
        goto error;
      }
    }
  }
});

DEFINE_IC(UnaryArith, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoUnaryArithFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(BinaryArith, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoBinaryArithFallback(cx, frame, fallback, value0, value1, &state.res)) {
    goto error;
  }
});

DEFINE_IC(ToBool, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoToBoolFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(Compare, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoCompareFallback(cx, frame, fallback, value0, value1, &state.res)) {
    goto error;
  }
});

DEFINE_IC(InstanceOf, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoInstanceOfFallback(cx, frame, fallback, value0, value1, &state.res)) {
    goto error;
  }
});

DEFINE_IC(In, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoInFallback(cx, frame, fallback, value0, value1, &state.res)) {
    goto error;
  }
});

DEFINE_IC(BindName, 1, {
  IC_LOAD_OBJ(obj0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoBindNameFallback(cx, frame, fallback, obj0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(SetProp, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoSetPropFallback(cx, frame, fallback, nullptr, value0, value1)) {
    goto error;
  }
});

DEFINE_IC(NewObject, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoNewObjectFallback(cx, frame, fallback, &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetProp, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetPropFallback(cx, frame, fallback, &value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetPropSuper, 2, {
  IC_LOAD_VAL(value0, 1);
  IC_LOAD_VAL(value1, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetPropSuperFallback(cx, frame, fallback, value0, &value1,
                              &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetElem, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetElemFallback(cx, frame, fallback, value0, value1, &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetElemSuper, 3, {
  IC_LOAD_VAL(value0, 0);  // receiver
  IC_LOAD_VAL(value1, 1);  // obj (lhs)
  IC_LOAD_VAL(value2, 2);  // key (rhs)
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetElemSuperFallback(cx, frame, fallback, value1, value2, value0,
                              &state.res)) {
    goto error;
  }
});

DEFINE_IC(NewArray, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoNewArrayFallback(cx, frame, fallback, &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetIntrinsic, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetIntrinsicFallback(cx, frame, fallback, &state.res)) {
    goto error;
  }
});

DEFINE_IC(SetElem, 3, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  IC_LOAD_VAL(value2, 2);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoSetElemFallback(cx, frame, fallback, nullptr, value0, value1,
                         value2)) {
    goto error;
  }
});

DEFINE_IC(HasOwn, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoHasOwnFallback(cx, frame, fallback, value0, value1, &state.res)) {
    goto error;
  }
});

DEFINE_IC(CheckPrivateField, 2, {
  IC_LOAD_VAL(value0, 0);
  IC_LOAD_VAL(value1, 1);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoCheckPrivateFieldFallback(cx, frame, fallback, value0, value1,
                                   &state.res)) {
    goto error;
  }
});

DEFINE_IC(GetIterator, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoGetIteratorFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(ToPropertyKey, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoToPropertyKeyFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(OptimizeSpreadCall, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoOptimizeSpreadCallFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(OptimizeGetIterator, 1, {
  IC_LOAD_VAL(value0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoOptimizeGetIteratorFallback(cx, frame, fallback, value0, &state.res)) {
    goto error;
  }
});

DEFINE_IC(Rest, 0, {
  PUSH_FALLBACK_IC_FRAME();
  if (!DoRestFallback(cx, frame, fallback, &state.res)) {
    goto error;
  }
});

DEFINE_IC(CloseIter, 1, {
  IC_LOAD_OBJ(obj0, 0);
  PUSH_FALLBACK_IC_FRAME();
  if (!DoCloseIterFallback(cx, frame, fallback, obj0)) {
    goto error;
  }
});

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
      if (script->hasDebugScript()) {                                     \
        goto debug;                                                       \
      }                                                                   \
    }
#else
#  define DEBUG_CHECK()
#endif

#define LABEL(op) (&&label_##op)
#define CASE(op) label_##op:
#if !defined(TRACE_INTERP)
#  define DISPATCH() \
    DEBUG_CHECK();   \
    goto* addresses[*pc]
#else
#  define DISPATCH() \
    DEBUG_CHECK();   \
    goto dispatch
#endif

#define ADVANCE(delta) pc += (delta);
#define ADVANCE_AND_DISPATCH(delta) \
  ADVANCE(delta);                   \
  DISPATCH();

#define END_OP(op) ADVANCE_AND_DISPATCH(JSOpLength_##op);

#define IC_SET_ARG_FROM_STACK(index, stack_index) \
  icregs.icVals[(index)] = sp[(stack_index)].asUInt64();
#define IC_POP_ARG(index) icregs.icVals[(index)] = (*sp++).asUInt64();
#define IC_SET_VAL_ARG(index, expr) icregs.icVals[(index)] = (expr).asRawBits();
#define IC_SET_OBJ_ARG(index, expr) \
  icregs.icVals[(index)] = reinterpret_cast<uint64_t>(expr);
#define IC_PUSH_RESULT() PUSH(StackVal(icregs.icResult));

#if !defined(TRACE_INTERP)
#  define PREDICT_NEXT(op)       \
    if (JSOp(*pc) == JSOp::op) { \
      DEBUG_CHECK();             \
      goto label_##op;           \
    }
#else
#  define PREDICT_NEXT(op)
#endif

#define COUNT_COVERAGE_PC(PC)                        \
  if (script->hasScriptCounts()) {                   \
    PCCounts* counts = script->maybeGetPCCounts(PC); \
    MOZ_ASSERT(counts);                              \
    counts->numExec()++;                             \
  }
#define COUNT_COVERAGE_MAIN()                                        \
  {                                                                  \
    jsbytecode* main = script->main();                               \
    if (!BytecodeIsJumpTarget(JSOp(*main))) COUNT_COVERAGE_PC(main); \
  }

#define NEXT_IC() frame->interpreterICEntry()++;

#define INVOKE_IC(kind)                                              \
  switch (IC##kind(frame, frameMgr, state, icregs, stack, sp, pc)) { \
    case PBIResult::Ok:                                              \
      break;                                                         \
    case PBIResult::Error:                                           \
      goto error;                                                    \
    case PBIResult::Unwind:                                          \
      goto unwind;                                                   \
    case PBIResult::UnwindError:                                     \
      goto unwind_error;                                             \
    case PBIResult::UnwindRet:                                       \
      goto unwind_ret;                                               \
  }                                                                  \
  NEXT_IC();

PBIResult PortableBaselineInterpret(JSContext* cx_, State& state, Stack& stack,
                                    StackVal* sp, JSObject* envChain,
                                    Value* ret) {
#define OPCODE_LABEL(op, ...) LABEL(op),
#define TRAILING_LABEL(v) LABEL(default),

  static const void* const addresses[EnableInterruptsPseudoOpcode + 1] = {
      FOR_EACH_OPCODE(OPCODE_LABEL)
          FOR_EACH_TRAILING_UNUSED_OPCODE(TRAILING_LABEL)};

#undef OPCODE_LABEL
#undef TRAILING_LABEL

  PUSHNATIVE(StackValNative(nullptr));  // Fake return address.
  BaselineFrame* frame = stack.pushFrame(sp, cx_, envChain);
  MOZ_ASSERT(frame);  // safety: stack margin.
  sp = reinterpret_cast<StackVal*>(frame);

  // Save the entry frame so that when unwinding, we know when to
  // return from this C++ frame.
  StackVal* entryFrame = sp;

  ICRegs icregs;
  RootedScript script(cx_, frame->script());
  jsbytecode* pc = frame->interpreterPC();
  bool from_unwind = false;

  VMFrameManager frameMgr(cx_, frame);

  AutoCheckRecursionLimit recursion(frameMgr.cxForLocalUseOnly());
  if (!recursion.checkDontReport(frameMgr.cxForLocalUseOnly())) {
    PUSH_EXIT_FRAME();
    ReportOverRecursed(frameMgr.cxForLocalUseOnly());
    return PBIResult::Error;
  }

  // Check max stack depth once, so we don't need to check it
  // otherwise below for ordinary stack-manipulation opcodes (just for
  // exit frames).
  if (!stack.check(sp, sizeof(StackVal) * script->nslots())) {
    PUSH_EXIT_FRAME();
    ReportOverRecursed(frameMgr.cxForLocalUseOnly());
    return PBIResult::Error;
  }

  uint32_t nfixed = script->nfixed();
  for (uint32_t i = 0; i < nfixed; i++) {
    PUSH(StackVal(UndefinedValue()));
  }
  ret->setUndefined();

  // Check if we are being debugged, and set a flag in the frame if so. This
  // flag must be set before calling InitFunctionEnvironmentObjects.
  if (script->isDebuggee()) {
    TRACE_PRINTF("Script is debuggee\n");
    frame->setIsDebuggee();
  }

  if (CalleeTokenIsFunction(frame->calleeToken())) {
    JSFunction* func = CalleeTokenToFunction(frame->calleeToken());
    frame->setEnvironmentChain(func->environment());
    if (func->needsFunctionEnvironmentObjects()) {
      PUSH_EXIT_FRAME();
      if (!js::InitFunctionEnvironmentObjects(cx, frame)) {
        goto error;
      }
      TRACE_PRINTF("callee is func %p; created environment object: %p\n", func,
                   frame->environmentChain());
    }
  }

  // The debug prologue can't run until the function environment is set up.
  if (script->isDebuggee()) {
    PUSH_EXIT_FRAME();
    if (!DebugPrologue(cx, frame)) {
      goto error;
    }
  }

  if (!script->hasScriptCounts()) {
    if (frameMgr.cxForLocalUseOnly()->realm()->collectCoverageForDebug()) {
      PUSH_EXIT_FRAME();
      if (!script->initScriptCounts(cx)) {
        goto error;
      }
    }
  }
  COUNT_COVERAGE_MAIN();

#ifndef __wasi__
  if (frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
    PUSH_EXIT_FRAME();
    if (!InterruptCheck(cx)) {
      goto error;
    }
  }
#endif

  TRACE_PRINTF("Entering: sp = %p fp = %p frame = %p, script = %p, pc = %p\n",
               sp, stack.fp, frame, script.get(), pc);
  TRACE_PRINTF("nslots = %d nfixed = %d\n", int(script->nslots()),
               int(script->nfixed()));

  while (true) {
    DEBUG_CHECK();

#ifndef __wasi__
  dispatch:
#endif

#ifdef TRACE_INTERP
  {
    JSOp op = JSOp(*pc);
    printf("sp[0] = %" PRIx64 " sp[1] = %" PRIx64 " sp[2] = %" PRIx64 "\n",
           sp[0].asUInt64(), sp[1].asUInt64(), sp[2].asUInt64());
    printf("script = %p pc = %p: %s (ic %d) pending = %d\n", script.get(), pc,
           CodeName(op),
           (int)(frame->interpreterICEntry() -
                 script->jitScript()->icScript()->icEntries()),
           frameMgr.cxForLocalUseOnly()->isExceptionPending());
    printf("sp = %p fp = %p\n", sp, stack.fp);
    printf("TOS tag: %d\n", int(sp[0].asValue().asRawBits() >> 47));
    fflush(stdout);
  }
#endif

    goto* addresses[*pc];

    CASE(Nop) { END_OP(Nop); }
    CASE(NopIsAssignOp) { END_OP(NopIsAssignOp); }
    CASE(Undefined) {
      PUSH(StackVal(UndefinedValue()));
      END_OP(Undefined);
    }
    CASE(Null) {
      PUSH(StackVal(NullValue()));
      END_OP(Null);
    }
    CASE(False) {
      PUSH(StackVal(BooleanValue(false)));
      END_OP(False);
    }
    CASE(True) {
      PUSH(StackVal(BooleanValue(true)));
      END_OP(True);
    }
    CASE(Int32) {
      PUSH(StackVal(Int32Value(GET_INT32(pc))));
      END_OP(Int32);
    }
    CASE(Zero) {
      PUSH(StackVal(Int32Value(0)));
      END_OP(Zero);
    }
    CASE(One) {
      PUSH(StackVal(Int32Value(1)));
      END_OP(One);
    }
    CASE(Int8) {
      PUSH(StackVal(Int32Value(GET_INT8(pc))));
      END_OP(Int8);
    }
    CASE(Uint16) {
      PUSH(StackVal(Int32Value(GET_UINT16(pc))));
      END_OP(Uint16);
    }
    CASE(Uint24) {
      PUSH(StackVal(Int32Value(GET_UINT24(pc))));
      END_OP(Uint24);
    }
    CASE(Double) {
      PUSH(StackVal(GET_INLINE_VALUE(pc)));
      END_OP(Double);
    }
    CASE(BigInt) {
      PUSH(StackVal(JS::BigIntValue(script->getBigInt(pc))));
      END_OP(BigInt);
    }
    CASE(String) {
      PUSH(StackVal(StringValue(script->getString(pc))));
      END_OP(String);
    }
    CASE(Symbol) {
      PUSH(StackVal(
          SymbolValue(frameMgr.cxForLocalUseOnly()->wellKnownSymbols().get(
              GET_UINT8(pc)))));
      END_OP(Symbol);
    }
    CASE(Void) {
      sp[0] = StackVal(JS::UndefinedValue());
      END_OP(Void);
    }

    CASE(Typeof)
    CASE(TypeofExpr) {
      static_assert(JSOpLength_Typeof == JSOpLength_TypeofExpr);
      if (kHybridICs) {
        sp[0] = StackVal(StringValue(TypeOfOperation(
            Stack::handle(sp), frameMgr.cxForLocalUseOnly()->runtime())));
        NEXT_IC();
      } else {
        IC_POP_ARG(0);
        INVOKE_IC(Typeof);
        IC_PUSH_RESULT();
      }
      END_OP(Typeof);
    }

    CASE(TypeofEq) {
      if (kHybridICs) {
        TypeofEqOperand operand = TypeofEqOperand::fromRawValue(GET_UINT8(pc));
        bool result = js::TypeOfValue(Stack::handle(sp)) == operand.type();
        if (operand.compareOp() == JSOp::Ne) {
          result = !result;
        }
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
      } else {
        IC_POP_ARG(0);
        INVOKE_IC(TypeofEq);
        IC_PUSH_RESULT();
      }
      END_OP(TypeofEq);
    }

    CASE(Pos) {
      if (sp[0].asValue().isNumber()) {
        // Nothing!
        NEXT_IC();
        END_OP(Pos);
      } else {
        goto generic_unary;
      }
    }
    CASE(Neg) {
      if (sp[0].asValue().isInt32()) {
        int32_t i = sp[0].asValue().toInt32();
        if (i != 0 && i != INT32_MIN) {
          sp[0] = StackVal(Int32Value(-i));
          NEXT_IC();
          END_OP(Neg);
        }
      }
      if (sp[0].asValue().isNumber()) {
        sp[0] = StackVal(NumberValue(-sp[0].asValue().toNumber()));
        NEXT_IC();
        END_OP(Neg);
      }
      goto generic_unary;
    }

    CASE(Inc) {
      if (sp[0].asValue().isInt32()) {
        int32_t i = sp[0].asValue().toInt32();
        if (i != INT32_MAX) {
          sp[0] = StackVal(Int32Value(i + 1));
          NEXT_IC();
          END_OP(Inc);
        }
      }
      if (sp[0].asValue().isNumber()) {
        sp[0] = StackVal(NumberValue(sp[0].asValue().toNumber() + 1));
        NEXT_IC();
        END_OP(Inc);
      }
      goto generic_unary;
    }
    CASE(Dec) {
      if (sp[0].asValue().isInt32()) {
        int32_t i = sp[0].asValue().toInt32();
        if (i != INT32_MIN) {
          sp[0] = StackVal(Int32Value(i - 1));
          NEXT_IC();
          END_OP(Dec);
        }
      }
      if (sp[0].asValue().isNumber()) {
        sp[0] = StackVal(NumberValue(sp[0].asValue().toNumber() - 1));
        NEXT_IC();
        END_OP(Dec);
      }
      goto generic_unary;
    }

    CASE(BitNot) {
      if (sp[0].asValue().isInt32()) {
        int32_t i = sp[0].asValue().toInt32();
        sp[0] = StackVal(Int32Value(~i));
        NEXT_IC();
        END_OP(Inc);
      }
      goto generic_unary;
    }

    CASE(ToNumeric) {
      if (sp[0].asValue().isNumeric()) {
        NEXT_IC();
      } else if (kHybridICs) {
        MutableHandleValue val = Stack::handleMut(&sp[0]);
        PUSH_EXIT_FRAME();
        if (!ToNumeric(cx, val)) {
          goto error;
        }
        NEXT_IC();
      } else {
        goto generic_unary;
      }
      END_OP(ToNumeric);
    }

  generic_unary: {
    static_assert(JSOpLength_Pos == JSOpLength_Neg);
    static_assert(JSOpLength_Pos == JSOpLength_BitNot);
    static_assert(JSOpLength_Pos == JSOpLength_Inc);
    static_assert(JSOpLength_Pos == JSOpLength_Dec);
    static_assert(JSOpLength_Pos == JSOpLength_ToNumeric);
    IC_POP_ARG(0);
    INVOKE_IC(UnaryArith);
    IC_PUSH_RESULT();
    END_OP(Pos);
  }

    CASE(Not) {
      if (kHybridICs) {
        sp[0] = StackVal(BooleanValue(!ToBoolean(Stack::handle(sp))));
        NEXT_IC();
      } else {
        IC_POP_ARG(0);
        INVOKE_IC(ToBool);
        PUSH(StackVal(
            BooleanValue(!Value::fromRawBits(icregs.icResult).toBoolean())));
      }
      END_OP(Not);
    }

    CASE(And) {
      bool result;
      if (kHybridICs) {
        result = ToBoolean(Stack::handle(sp));
        NEXT_IC();
      } else {
        IC_SET_ARG_FROM_STACK(0, 0);
        INVOKE_IC(ToBool);
        result = Value::fromRawBits(icregs.icResult).toBoolean();
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
      if (kHybridICs) {
        result = ToBoolean(Stack::handle(sp));
        NEXT_IC();
      } else {
        IC_SET_ARG_FROM_STACK(0, 0);
        INVOKE_IC(ToBool);
        result = Value::fromRawBits(icregs.icResult).toBoolean();
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
      if (kHybridICs) {
        result = ToBoolean(Stack::handle(sp));
        POP();
        NEXT_IC();
      } else {
        IC_POP_ARG(0);
        INVOKE_IC(ToBool);
        result = Value::fromRawBits(icregs.icResult).toBoolean();
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
      if (kHybridICs) {
        result = ToBoolean(Stack::handle(sp));
        POP();
        NEXT_IC();
      } else {
        IC_POP_ARG(0);
        INVOKE_IC(ToBool);
        result = Value::fromRawBits(icregs.icResult).toBoolean();
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
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int64_t lhs = sp[1].asValue().toInt32();
        int64_t rhs = sp[0].asValue().toInt32();
        if (lhs + rhs >= int64_t(INT32_MIN) &&
            lhs + rhs <= int64_t(INT32_MAX)) {
          POP();
          sp[0] = StackVal(Int32Value(int32_t(lhs + rhs)));
          NEXT_IC();
          END_OP(Add);
        }
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        POP();
        sp[0] = StackVal(NumberValue(lhs + rhs));
        NEXT_IC();
        END_OP(Add);
      }
      if (kHybridICs) {
        MutableHandleValue lhs = Stack::handleMut(sp + 1);
        MutableHandleValue rhs = Stack::handleMut(sp);
        MutableHandleValue result = Stack::handleMut(sp + 1);
        {
          PUSH_EXIT_FRAME();
          if (!AddOperation(cx, lhs, rhs, result)) {
            goto error;
          }
        }
        POP();
        NEXT_IC();
        END_OP(Add);
      }
      goto generic_binary;
    }

    CASE(Sub) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int64_t lhs = sp[1].asValue().toInt32();
        int64_t rhs = sp[0].asValue().toInt32();
        if (lhs - rhs >= int64_t(INT32_MIN) &&
            lhs - rhs <= int64_t(INT32_MAX)) {
          POP();
          sp[0] = StackVal(Int32Value(int32_t(lhs - rhs)));
          NEXT_IC();
          END_OP(Sub);
        }
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        POP();
        sp[0] = StackVal(NumberValue(lhs - rhs));
        NEXT_IC();
        END_OP(Add);
      }
      if (kHybridICs) {
        MutableHandleValue lhs = Stack::handleMut(sp + 1);
        MutableHandleValue rhs = Stack::handleMut(sp);
        MutableHandleValue result = Stack::handleMut(sp + 1);
        {
          PUSH_EXIT_FRAME();
          if (!SubOperation(cx, lhs, rhs, result)) {
            goto error;
          }
        }
        POP();
        NEXT_IC();
        END_OP(Sub);
      }
      goto generic_binary;
    }

    CASE(Mul) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int64_t lhs = sp[1].asValue().toInt32();
        int64_t rhs = sp[0].asValue().toInt32();
        int64_t product = lhs * rhs;
        if (product >= int64_t(INT32_MIN) && product <= int64_t(INT32_MAX) &&
            (product != 0 || !((lhs < 0) ^ (rhs < 0)))) {
          POP();
          sp[0] = StackVal(Int32Value(int32_t(product)));
          NEXT_IC();
          END_OP(Mul);
        }
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        POP();
        sp[0] = StackVal(NumberValue(lhs * rhs));
        NEXT_IC();
        END_OP(Mul);
      }
      if (kHybridICs) {
        MutableHandleValue lhs = Stack::handleMut(sp + 1);
        MutableHandleValue rhs = Stack::handleMut(sp);
        MutableHandleValue result = Stack::handleMut(sp + 1);
        {
          PUSH_EXIT_FRAME();
          if (!MulOperation(cx, lhs, rhs, result)) {
            goto error;
          }
        }
        POP();
        NEXT_IC();
        END_OP(Mul);
      }
      goto generic_binary;
    }
    CASE(Div) {
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        POP();
        sp[0] = StackVal(NumberValue(NumberDiv(lhs, rhs)));
        NEXT_IC();
        END_OP(Div);
      }
      if (kHybridICs) {
        MutableHandleValue lhs = Stack::handleMut(sp + 1);
        MutableHandleValue rhs = Stack::handleMut(sp);
        MutableHandleValue result = Stack::handleMut(sp + 1);
        {
          PUSH_EXIT_FRAME();
          if (!DivOperation(cx, lhs, rhs, result)) {
            goto error;
          }
        }
        POP();
        NEXT_IC();
        END_OP(Div);
      }
      goto generic_binary;
    }
    CASE(Mod) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int64_t lhs = sp[1].asValue().toInt32();
        int64_t rhs = sp[0].asValue().toInt32();
        if (lhs > 0 && rhs > 0) {
          int64_t mod = lhs % rhs;
          POP();
          sp[0] = StackVal(Int32Value(int32_t(mod)));
          NEXT_IC();
          END_OP(Mod);
        }
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        POP();
        sp[0] = StackVal(DoubleValue(NumberMod(lhs, rhs)));
        NEXT_IC();
        END_OP(Mod);
      }
      if (kHybridICs) {
        MutableHandleValue lhs = Stack::handleMut(sp + 1);
        MutableHandleValue rhs = Stack::handleMut(sp);
        MutableHandleValue result = Stack::handleMut(sp + 1);
        {
          PUSH_EXIT_FRAME();
          if (!ModOperation(cx, lhs, rhs, result)) {
            goto error;
          }
        }
        POP();
        NEXT_IC();
        END_OP(Mod);
      }
      goto generic_binary;
    }
    CASE(Pow) {
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        POP();
        sp[0] = StackVal(NumberValue(ecmaPow(lhs, rhs)));
        NEXT_IC();
        END_OP(Pow);
      }
      if (kHybridICs) {
        MutableHandleValue lhs = Stack::handleMut(sp + 1);
        MutableHandleValue rhs = Stack::handleMut(sp);
        MutableHandleValue result = Stack::handleMut(sp + 1);
        {
          PUSH_EXIT_FRAME();
          if (!PowOperation(cx, lhs, rhs, result)) {
            goto error;
          }
        }
        POP();
        NEXT_IC();
        END_OP(Pow);
      }
      goto generic_binary;
    }
    CASE(BitOr) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int32_t lhs = sp[1].asValue().toInt32();
        int32_t rhs = sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(Int32Value(lhs | rhs));
        NEXT_IC();
        END_OP(BitOr);
      }
      goto generic_binary;
    }
    CASE(BitAnd) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int32_t lhs = sp[1].asValue().toInt32();
        int32_t rhs = sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(Int32Value(lhs & rhs));
        NEXT_IC();
        END_OP(BitAnd);
      }
      goto generic_binary;
    }
    CASE(BitXor) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int32_t lhs = sp[1].asValue().toInt32();
        int32_t rhs = sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(Int32Value(lhs ^ rhs));
        NEXT_IC();
        END_OP(BitXor);
      }
      goto generic_binary;
    }
    CASE(Lsh) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        // Unsigned to avoid undefined behavior on left-shift overflow
        // (see comment in BitLshOperation in Interpreter.cpp).
        uint32_t lhs = uint32_t(sp[1].asValue().toInt32());
        uint32_t rhs = uint32_t(sp[0].asValue().toInt32());
        POP();
        rhs &= 31;
        sp[0] = StackVal(Int32Value(int32_t(lhs << rhs)));
        NEXT_IC();
        END_OP(Lsh);
      }
      goto generic_binary;
    }
    CASE(Rsh) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        int32_t lhs = sp[1].asValue().toInt32();
        int32_t rhs = sp[0].asValue().toInt32();
        POP();
        rhs &= 31;
        sp[0] = StackVal(Int32Value(lhs >> rhs));
        NEXT_IC();
        END_OP(Rsh);
      }
      goto generic_binary;
    }
    CASE(Ursh) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        uint32_t lhs = uint32_t(sp[1].asValue().toInt32());
        int32_t rhs = sp[0].asValue().toInt32();
        POP();
        rhs &= 31;
        uint32_t result = lhs >> rhs;
        if (result <= uint32_t(INT32_MAX)) {
          sp[0] = StackVal(Int32Value(int32_t(result)));
        } else {
          sp[0] = StackVal(NumberValue(double(result)));
        }
        NEXT_IC();
        END_OP(Ursh);
      }
      goto generic_binary;
    }

  generic_binary: {
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
    INVOKE_IC(BinaryArith);
    IC_PUSH_RESULT();
    END_OP(Div);
  }

    CASE(Eq) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        bool result = sp[0].asValue().toInt32() == sp[1].asValue().toInt32();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Eq);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        bool result = lhs == rhs;
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Eq);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        bool result = sp[0].asValue().toNumber() == sp[1].asValue().toNumber();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Eq);
      }
      goto generic_cmp;
    }

    CASE(Ne) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        bool result = sp[0].asValue().toInt32() != sp[1].asValue().toInt32();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Ne);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        bool result = lhs != rhs;
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Ne);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        bool result = sp[0].asValue().toNumber() != sp[1].asValue().toNumber();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Eq);
      }
      goto generic_cmp;
    }

    CASE(Lt) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        bool result = sp[1].asValue().toInt32() < sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Lt);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        bool result = lhs < rhs;
        if (std::isnan(lhs) || std::isnan(rhs)) {
          result = false;
        }
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Lt);
      }
      goto generic_cmp;
    }
    CASE(Le) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        bool result = sp[1].asValue().toInt32() <= sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Le);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        bool result = lhs <= rhs;
        if (std::isnan(lhs) || std::isnan(rhs)) {
          result = false;
        }
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Le);
      }
      goto generic_cmp;
    }
    CASE(Gt) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        bool result = sp[1].asValue().toInt32() > sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Gt);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        bool result = lhs > rhs;
        if (std::isnan(lhs) || std::isnan(rhs)) {
          result = false;
        }
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Gt);
      }
      goto generic_cmp;
    }
    CASE(Ge) {
      if (sp[0].asValue().isInt32() && sp[1].asValue().isInt32()) {
        bool result = sp[1].asValue().toInt32() >= sp[0].asValue().toInt32();
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Ge);
      }
      if (sp[0].asValue().isNumber() && sp[1].asValue().isNumber()) {
        double lhs = sp[1].asValue().toNumber();
        double rhs = sp[0].asValue().toNumber();
        bool result = lhs >= rhs;
        if (std::isnan(lhs) || std::isnan(rhs)) {
          result = false;
        }
        POP();
        sp[0] = StackVal(BooleanValue(result));
        NEXT_IC();
        END_OP(Ge);
      }
      goto generic_cmp;
    }

    CASE(StrictEq)
    CASE(StrictNe) {
      if (kHybridICs) {
        bool result;
        HandleValue lval = Stack::handle(sp + 1);
        HandleValue rval = Stack::handle(sp);
        if (sp[0].asValue().isString() && sp[1].asValue().isString()) {
          PUSH_EXIT_FRAME();
          if (!js::StrictlyEqual(cx, lval, rval, &result)) {
            goto error;
          }
        } else {
          if (!js::StrictlyEqual(nullptr, lval, rval, &result)) {
            goto error;
          }
        }
        POP();
        sp[0] = StackVal(
            BooleanValue((JSOp(*pc) == JSOp::StrictEq) ? result : !result));
        NEXT_IC();
        END_OP(StrictEq);
      } else {
        goto generic_cmp;
      }
    }

  generic_cmp: {
    static_assert(JSOpLength_Eq == JSOpLength_Ne);
    static_assert(JSOpLength_Eq == JSOpLength_StrictEq);
    static_assert(JSOpLength_Eq == JSOpLength_StrictNe);
    static_assert(JSOpLength_Eq == JSOpLength_Lt);
    static_assert(JSOpLength_Eq == JSOpLength_Gt);
    static_assert(JSOpLength_Eq == JSOpLength_Le);
    static_assert(JSOpLength_Eq == JSOpLength_Ge);
    IC_POP_ARG(1);
    IC_POP_ARG(0);
    INVOKE_IC(Compare);
    IC_PUSH_RESULT();
    END_OP(Eq);
  }

    CASE(Instanceof) {
      IC_POP_ARG(1);
      IC_POP_ARG(0);
      INVOKE_IC(InstanceOf);
      IC_PUSH_RESULT();
      END_OP(Instanceof);
    }

    CASE(In) {
      IC_POP_ARG(1);
      IC_POP_ARG(0);
      INVOKE_IC(In);
      IC_PUSH_RESULT();
      END_OP(In);
    }

    CASE(ToPropertyKey) {
      IC_POP_ARG(0);
      INVOKE_IC(ToPropertyKey);
      IC_PUSH_RESULT();
      END_OP(ToPropertyKey);
    }

    CASE(ToString) {
      if (sp[0].asValue().isString()) {
        END_OP(ToString);
      }
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        if (JSString* result =
                ToStringSlow<NoGC>(frameMgr.cxForLocalUseOnly(), value0)) {
          PUSH(StackVal(StringValue(result)));
        } else {
          {
            PUSH_EXIT_FRAME();
            result = ToString<CanGC>(cx, value0);
            if (!result) {
              goto error;
            }
          }
          PUSH(StackVal(StringValue(result)));
        }
      }
      END_OP(ToString);
    }

    CASE(IsNullOrUndefined) {
      bool result = sp[0].asValue().isNull() || sp[0].asValue().isUndefined();
      PUSH(StackVal(BooleanValue(result)));
      END_OP(IsNullOrUndefined);
    }

    CASE(GlobalThis) {
      PUSH(StackVal(ObjectValue(*frameMgr.cxForLocalUseOnly()
                                     ->global()
                                     ->lexicalEnvironment()
                                     .thisObject())));
      END_OP(GlobalThis);
    }

    CASE(NonSyntacticGlobalThis) {
      {
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        ReservedRooted<Value> value0(&state.value0);
        {
          PUSH_EXIT_FRAME();
          js::GetNonSyntacticGlobalThis(cx, obj0, &value0);
        }
        PUSH(StackVal(value0));
      }
      END_OP(NonSyntacticGlobalThis);
    }

    CASE(NewTarget) {
      PUSH(StackVal(frame->newTarget()));
      END_OP(NewTarget);
    }

    CASE(DynamicImport) {
      {
        ReservedRooted<Value> value0(&state.value0,
                                     POP().asValue());  // options
        ReservedRooted<Value> value1(&state.value1,
                                     POP().asValue());  // specifier
        JSObject* promise;
        {
          PUSH_EXIT_FRAME();
          promise = StartDynamicModuleImport(cx, script, value1, value0);
          if (!promise) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*promise)));
      }
      END_OP(DynamicImport);
    }

    CASE(ImportMeta) {
      JSObject* metaObject;
      {
        PUSH_EXIT_FRAME();
        metaObject = ImportMetaOperation(cx, script);
        if (!metaObject) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*metaObject)));
      END_OP(ImportMeta);
    }

    CASE(NewInit) {
      if (kHybridICs) {
        JSObject* obj;
        {
          PUSH_EXIT_FRAME();
          obj = NewObjectOperation(cx, script, pc);
          if (!obj) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*obj)));
        NEXT_IC();
        END_OP(NewInit);
      } else {
        INVOKE_IC(NewObject);
        IC_PUSH_RESULT();
        END_OP(NewInit);
      }
    }
    CASE(NewObject) {
      if (kHybridICs) {
        JSObject* obj;
        {
          PUSH_EXIT_FRAME();
          obj = NewObjectOperation(cx, script, pc);
          if (!obj) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*obj)));
        NEXT_IC();
        END_OP(NewObject);
      } else {
        INVOKE_IC(NewObject);
        IC_PUSH_RESULT();
        END_OP(NewObject);
      }
    }
    CASE(Object) {
      PUSH(StackVal(ObjectValue(*script->getObject(pc))));
      END_OP(Object);
    }
    CASE(ObjWithProto) {
      {
        ReservedRooted<Value> value0(&state.value0, sp[0].asValue());
        JSObject* obj;
        {
          PUSH_EXIT_FRAME();
          obj = ObjectWithProtoOperation(cx, value0);
          if (!obj) {
            goto error;
          }
        }
        sp[0] = StackVal(ObjectValue(*obj));
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
      StackVal val = sp[0];
      IC_POP_ARG(2);
      IC_POP_ARG(1);
      IC_SET_ARG_FROM_STACK(0, 0);
      if (JSOp(*pc) == JSOp::SetElem || JSOp(*pc) == JSOp::StrictSetElem) {
        sp[0] = val;
      }
      INVOKE_IC(SetElem);
      if (JSOp(*pc) == JSOp::InitElemInc) {
        PUSH(StackVal(
            Int32Value(Value::fromRawBits(icregs.icVals[1]).toInt32() + 1)));
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
        ReservedRooted<JSObject*> obj1(&state.obj1,
                                       &POP().asValue().toObject());  // val
        ReservedRooted<JSObject*> obj0(
            &state.obj0, &sp[0].asValue().toObject());  // obj; leave on stack
        ReservedRooted<PropertyName*> name0(&state.name0, script->getName(pc));
        {
          PUSH_EXIT_FRAME();
          if (!InitPropGetterSetterOperation(cx, pc, obj0, name0, obj1)) {
            goto error;
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
        ReservedRooted<JSObject*> obj1(&state.obj1,
                                       &POP().asValue().toObject());   // val
        ReservedRooted<Value> value0(&state.value0, POP().asValue());  // idval
        ReservedRooted<JSObject*> obj0(
            &state.obj0, &sp[0].asValue().toObject());  // obj; leave on stack
        {
          PUSH_EXIT_FRAME();
          if (!InitElemGetterSetterOperation(cx, pc, obj0, value0, obj1)) {
            goto error;
          }
        }
      }
      END_OP(InitElemGetter);
    }

    CASE(GetProp)
    CASE(GetBoundName) {
      static_assert(JSOpLength_GetProp == JSOpLength_GetBoundName);
      IC_POP_ARG(0);
      INVOKE_IC(GetProp);
      IC_PUSH_RESULT();
      END_OP(GetProp);
    }
    CASE(GetPropSuper) {
      IC_POP_ARG(0);
      IC_POP_ARG(1);
      INVOKE_IC(GetPropSuper);
      IC_PUSH_RESULT();
      END_OP(GetPropSuper);
    }

    CASE(GetElem) {
      HandleValue lhs = Stack::handle(&sp[1]);
      HandleValue rhs = Stack::handle(&sp[0]);
      uint32_t index;
      if (IsDefinitelyIndex(rhs, &index)) {
        if (lhs.isString()) {
          JSString* str = lhs.toString();
          if (index < str->length() && str->isLinear()) {
            JSLinearString* linear = &str->asLinear();
            char16_t c = linear->latin1OrTwoByteChar(index);
            StaticStrings& sstr = frameMgr.cxForLocalUseOnly()->staticStrings();
            if (sstr.hasUnit(c)) {
              sp[1] = StackVal(StringValue(sstr.getUnit(c)));
              POP();
              NEXT_IC();
              END_OP(GetElem);
            }
          }
        }
        if (lhs.isObject()) {
          JSObject* obj = &lhs.toObject();
          Value ret;
          if (GetElementNoGC(frameMgr.cxForLocalUseOnly(), obj, lhs, index,
                             &ret)) {
            sp[1] = StackVal(ret);
            POP();
            NEXT_IC();
            END_OP(GetElem);
          }
        }
      }

      IC_POP_ARG(1);
      IC_POP_ARG(0);
      INVOKE_IC(GetElem);
      IC_PUSH_RESULT();
      END_OP(GetElem);
    }

    CASE(GetElemSuper) {
      // N.B.: second and third args are out of order! See the saga at
      // https://bugzilla.mozilla.org/show_bug.cgi?id=1709328; this is
      // an echo of that issue.
      IC_POP_ARG(1);
      IC_POP_ARG(2);
      IC_POP_ARG(0);
      INVOKE_IC(GetElemSuper);
      IC_PUSH_RESULT();
      END_OP(GetElemSuper);
    }

    CASE(DelProp) {
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        ReservedRooted<PropertyName*> name0(&state.name0, script->getName(pc));
        bool res = false;
        {
          PUSH_EXIT_FRAME();
          if (!DelPropOperation<false>(cx, value0, name0, &res)) {
            goto error;
          }
        }
        PUSH(StackVal(BooleanValue(res)));
      }
      END_OP(DelProp);
    }
    CASE(StrictDelProp) {
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        ReservedRooted<PropertyName*> name0(&state.name0, script->getName(pc));
        bool res = false;
        {
          PUSH_EXIT_FRAME();
          if (!DelPropOperation<true>(cx, value0, name0, &res)) {
            goto error;
          }
        }
        PUSH(StackVal(BooleanValue(res)));
      }
      END_OP(StrictDelProp);
    }
    CASE(DelElem) {
      {
        ReservedRooted<Value> value1(&state.value1, POP().asValue());
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        bool res = false;
        {
          PUSH_EXIT_FRAME();
          if (!DelElemOperation<false>(cx, value0, value1, &res)) {
            goto error;
          }
        }
        PUSH(StackVal(BooleanValue(res)));
      }
      END_OP(DelElem);
    }
    CASE(StrictDelElem) {
      {
        ReservedRooted<Value> value1(&state.value1, POP().asValue());
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        bool res = false;
        {
          PUSH_EXIT_FRAME();
          if (!DelElemOperation<true>(cx, value0, value1, &res)) {
            goto error;
          }
        }
        PUSH(StackVal(BooleanValue(res)));
      }
      END_OP(StrictDelElem);
    }

    CASE(HasOwn) {
      IC_POP_ARG(1);
      IC_POP_ARG(0);
      INVOKE_IC(HasOwn);
      IC_PUSH_RESULT();
      END_OP(HasOwn);
    }

    CASE(CheckPrivateField) {
      IC_SET_ARG_FROM_STACK(1, 0);
      IC_SET_ARG_FROM_STACK(0, 1);
      INVOKE_IC(CheckPrivateField);
      IC_PUSH_RESULT();
      END_OP(CheckPrivateField);
    }

    CASE(NewPrivateName) {
      {
        ReservedRooted<JSAtom*> atom0(&state.atom0, script->getAtom(pc));
        JS::Symbol* symbol;
        {
          PUSH_EXIT_FRAME();
          symbol = NewPrivateName(cx, atom0);
          if (!symbol) {
            goto error;
          }
        }
        PUSH(StackVal(SymbolValue(symbol)));
      }
      END_OP(NewPrivateName);
    }

    CASE(SuperBase) {
      JSFunction& superEnvFunc = POP().asValue().toObject().as<JSFunction>();
      MOZ_ASSERT(superEnvFunc.allowSuperProperty());
      MOZ_ASSERT(superEnvFunc.baseScript()->needsHomeObject());
      const Value& homeObjVal = superEnvFunc.getExtendedSlot(
          FunctionExtended::METHOD_HOMEOBJECT_SLOT);

      JSObject* homeObj = &homeObjVal.toObject();
      JSObject* superBase = HomeObjectSuperBase(homeObj);

      PUSH(StackVal(ObjectOrNullValue(superBase)));
      END_OP(SuperBase);
    }

    CASE(SetPropSuper)
    CASE(StrictSetPropSuper) {
      // stack signature: receiver, lval, rval => rval
      static_assert(JSOpLength_SetPropSuper == JSOpLength_StrictSetPropSuper);
      bool strict = JSOp(*pc) == JSOp::StrictSetPropSuper;
      {
        ReservedRooted<Value> value2(&state.value2, POP().asValue());  // rval
        ReservedRooted<Value> value1(&state.value1, POP().asValue());  // lval
        ReservedRooted<Value> value0(&state.value0,
                                     POP().asValue());  // recevier
        ReservedRooted<PropertyName*> name0(&state.name0, script->getName(pc));
        {
          PUSH_EXIT_FRAME();
          // SetPropertySuper(cx, lval, receiver, name, rval, strict)
          // (N.B.: lval and receiver are transposed!)
          if (!SetPropertySuper(cx, value1, value0, name0, value2, strict)) {
            goto error;
          }
        }
        PUSH(StackVal(value2));
      }
      END_OP(SetPropSuper);
    }

    CASE(SetElemSuper)
    CASE(StrictSetElemSuper) {
      // stack signature: receiver, key, lval, rval => rval
      static_assert(JSOpLength_SetElemSuper == JSOpLength_StrictSetElemSuper);
      bool strict = JSOp(*pc) == JSOp::StrictSetElemSuper;
      {
        ReservedRooted<Value> value3(&state.value3, POP().asValue());  // rval
        ReservedRooted<Value> value2(&state.value2, POP().asValue());  // lval
        ReservedRooted<Value> value1(&state.value1, POP().asValue());  // index
        ReservedRooted<Value> value0(&state.value0,
                                     POP().asValue());  // receiver
        {
          PUSH_EXIT_FRAME();
          // SetElementSuper(cx, lval, receiver, index, rval, strict)
          // (N.B.: lval, receiver and index are rotated!)
          if (!SetElementSuper(cx, value2, value0, value1, value3, strict)) {
            goto error;
          }
        }
        PUSH(StackVal(value3));  // value
      }
      END_OP(SetElemSuper);
    }

    CASE(Iter) {
      IC_POP_ARG(0);
      INVOKE_IC(GetIterator);
      IC_PUSH_RESULT();
      END_OP(Iter);
    }

    CASE(MoreIter) {
      // iter => iter, name
      Value v = IteratorMore(&sp[0].asValue().toObject());
      PUSH(StackVal(v));
      END_OP(MoreIter);
    }

    CASE(IsNoIter) {
      // iter => iter, bool
      bool result = sp[0].asValue().isMagic(JS_NO_ITER_VALUE);
      PUSH(StackVal(BooleanValue(result)));
      END_OP(IsNoIter);
    }

    CASE(EndIter) {
      // iter, interval =>
      POP();
      CloseIterator(&POP().asValue().toObject());
      END_OP(EndIter);
    }

    CASE(CloseIter) {
      IC_SET_OBJ_ARG(0, &POP().asValue().toObject());
      INVOKE_IC(CloseIter);
      END_OP(CloseIter);
    }

    CASE(CheckIsObj) {
      if (!sp[0].asValue().isObject()) {
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(
            js::ThrowCheckIsObject(cx, js::CheckIsObjectKind(GET_UINT8(pc))));
        /* abandon frame; error handler will re-establish sp */
        goto error;
      }
      END_OP(CheckIsObj);
    }

    CASE(CheckObjCoercible) {
      {
        ReservedRooted<Value> value0(&state.value0, sp[0].asValue());
        if (value0.isNullOrUndefined()) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(ThrowObjectCoercible(cx, value0));
          /* abandon frame; error handler will re-establish sp */
          goto error;
        }
      }
      END_OP(CheckObjCoercible);
    }

    CASE(ToAsyncIter) {
      // iter, next => asynciter
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());  // next
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &POP().asValue().toObject());  // iter
        JSObject* result;
        {
          PUSH_EXIT_FRAME();
          result = CreateAsyncFromSyncIterator(cx, obj0, value0);
          if (!result) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*result)));
      }
      END_OP(ToAsyncIter);
    }

    CASE(MutateProto) {
      // obj, protoVal => obj
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &sp[0].asValue().toObject());
        {
          PUSH_EXIT_FRAME();
          if (!MutatePrototype(cx, obj0.as<PlainObject>(), value0)) {
            goto error;
          }
        }
      }
      END_OP(MutateProto);
    }

    CASE(NewArray) {
      if (kHybridICs) {
        ArrayObject* obj;
        {
          PUSH_EXIT_FRAME();
          uint32_t length = GET_UINT32(pc);
          obj = NewArrayOperation(cx, length);
          if (!obj) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*obj)));
        NEXT_IC();
        END_OP(NewArray);
      } else {
        INVOKE_IC(NewArray);
        IC_PUSH_RESULT();
        END_OP(NewArray);
      }
    }

    CASE(InitElemArray) {
      // array, val => array
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &sp[0].asValue().toObject());
        {
          PUSH_EXIT_FRAME();
          InitElemArrayOperation(cx, pc, obj0.as<ArrayObject>(), value0);
        }
      }
      END_OP(InitElemArray);
    }

    CASE(Hole) {
      PUSH(StackVal(MagicValue(JS_ELEMENTS_HOLE)));
      END_OP(Hole);
    }

    CASE(RegExp) {
      JSObject* obj;
      {
        PUSH_EXIT_FRAME();
        ReservedRooted<JSObject*> obj0(&state.obj0, script->getRegExp(pc));
        obj = CloneRegExpObject(cx, obj0.as<RegExpObject>());
        if (!obj) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*obj)));
      END_OP(RegExp);
    }

    CASE(Lambda) {
      {
        ReservedRooted<JSFunction*> fun0(&state.fun0, script->getFunction(pc));
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        JSObject* res;
        {
          PUSH_EXIT_FRAME();
          res = js::Lambda(cx, fun0, obj0);
          if (!res) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*res)));
      }
      END_OP(Lambda);
    }

    CASE(SetFunName) {
      // fun, name => fun
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());  // name
        ReservedRooted<JSFunction*> fun0(
            &state.fun0, &sp[0].asValue().toObject().as<JSFunction>());
        FunctionPrefixKind prefixKind = FunctionPrefixKind(GET_UINT8(pc));
        {
          PUSH_EXIT_FRAME();
          if (!SetFunctionName(cx, fun0, value0, prefixKind)) {
            goto error;
          }
        }
      }
      END_OP(SetFunName);
    }

    CASE(InitHomeObject) {
      // fun, homeObject => fun
      {
        ReservedRooted<JSObject*> obj0(
            &state.obj0, &POP().asValue().toObject());  // homeObject
        ReservedRooted<JSFunction*> fun0(
            &state.fun0, &sp[0].asValue().toObject().as<JSFunction>());
        MOZ_ASSERT(fun0->allowSuperProperty());
        MOZ_ASSERT(obj0->is<PlainObject>() || obj0->is<JSFunction>());
        fun0->setExtendedSlot(FunctionExtended::METHOD_HOMEOBJECT_SLOT,
                              ObjectValue(*obj0));
      }
      END_OP(InitHomeObject);
    }

    CASE(CheckClassHeritage) {
      {
        ReservedRooted<Value> value0(&state.value0, sp[0].asValue());
        {
          PUSH_EXIT_FRAME();
          if (!CheckClassHeritageOperation(cx, value0)) {
            goto error;
          }
        }
      }
      END_OP(CheckClassHeritage);
    }

    CASE(FunWithProto) {
      // proto => obj
      {
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &POP().asValue().toObject());  // proto
        ReservedRooted<JSObject*> obj1(&state.obj1, frame->environmentChain());
        ReservedRooted<JSFunction*> fun0(&state.fun0, script->getFunction(pc));
        JSObject* obj;
        {
          PUSH_EXIT_FRAME();
          obj = FunWithProtoOperation(cx, fun0, obj1, obj0);
          if (!obj) {
            goto error;
          }
        }
        PUSH(StackVal(ObjectValue(*obj)));
      }
      END_OP(FunWithProto);
    }

    CASE(BuiltinObject) {
      auto kind = BuiltinObjectKind(GET_UINT8(pc));
      JSObject* builtin;
      {
        PUSH_EXIT_FRAME();
        builtin = BuiltinObjectOperation(cx, kind);
        if (!builtin) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*builtin)));
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
      bool constructing =
          (op == JSOp::New || op == JSOp::NewContent || op == JSOp::SuperCall);
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
          ReservedRooted<JSFunction*> func(&state.fun0,
                                           &callee.toObject().as<JSFunction>());
          if (!func->hasBaseScript() || !func->isInterpreted()) {
            TRACE_PRINTF("missed fastpath: not an interpreted script\n");
            break;
          }
          if (!constructing && func->isClassConstructor()) {
            TRACE_PRINTF("missed fastpath: constructor called without `new`\n");
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
          if (frameMgr.cxForLocalUseOnly()->realm() != calleeScript->realm()) {
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

          if (!stack.check(sp, sizeof(StackVal) * (totalArgs + 3))) {
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
              // CreateThis might discard the JitScript but we're counting on it
              // continuing to exist while we evaluate the fastpath.
              AutoKeepJitScripts keepJitScript(cx);
              if (!CreateThis(cx, func, obj0, GenericObject, thisv)) {
                goto error;
              }

              TRACE_PRINTF("created %" PRIx64 "\n", thisv.get().asRawBits());
            }
          }

          // 0. Save current PC in current frame, so we can retrieve
          // it later.
          frame->interpreterPC() = pc;

          // 1. Push a baseline stub frame. Don't use the frame manager
          // -- we don't want the frame to be auto-freed when we leave
          // this scope, and we don't want to shadow `sp`.
          StackVal* exitFP = stack.pushExitFrame(sp, frame);
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
            PUSH(origArgs[i]);
          }

          // 4. Push inter-frame content: callee token, descriptor for
          // above.
          PUSHNATIVE(StackValNative(CalleeToToken(func, constructing)));
          PUSHNATIVE(StackValNative(
              MakeFrameDescriptorForJitCall(FrameType::BaselineStub, argc)));

          // 5. Push fake return address, set script, push baseline frame.
          PUSHNATIVE(StackValNative(nullptr));
          script.set(calleeScript);
          BaselineFrame* newFrame =
              stack.pushFrame(sp, frameMgr.cxForLocalUseOnly(),
                              /* envChain = */ func->environment());
          MOZ_ASSERT(newFrame);  // safety: stack margin.
          TRACE_PRINTF("callee frame at %p\n", newFrame);
          frame = newFrame;
          frameMgr.switchToFrame(frame);
          // 6. Set up PC and SP for callee.
          sp = reinterpret_cast<StackVal*>(frame);
          pc = calleeScript->code();
          // 7. Check callee stack space for max stack depth.
          if (!stack.check(sp, sizeof(StackVal) * calleeScript->nslots())) {
            PUSH_EXIT_FRAME();
            ReportOverRecursed(frameMgr.cxForLocalUseOnly());
            goto error;
          }
          // 8. Push local slots, and set return value to `undefined` by
          // default.
          uint32_t nfixed = calleeScript->nfixed();
          for (uint32_t i = 0; i < nfixed; i++) {
            PUSH(StackVal(UndefinedValue()));
          }
          ret->setUndefined();
          // 9. Initialize environment objects.
          if (func->needsFunctionEnvironmentObjects()) {
            PUSH_EXIT_FRAME();
            if (!js::InitFunctionEnvironmentObjects(cx, frame)) {
              goto error;
            }
          }
          // 10. Set debug flag, if appropriate.
          if (script->isDebuggee()) {
            TRACE_PRINTF("Script is debuggee\n");
            frame->setIsDebuggee();

            PUSH_EXIT_FRAME();
            if (!DebugPrologue(cx, frame)) {
              goto error;
            }
          }
          // 11. Check for interrupts.
#ifndef __wasi__
          if (frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
            PUSH_EXIT_FRAME();
            if (!InterruptCheck(cx)) {
              goto error;
            }
          }
#endif
          // 12. Initialize coverage tables, if needed.
          if (!script->hasScriptCounts()) {
            if (frameMgr.cxForLocalUseOnly()
                    ->realm()
                    ->collectCoverageForDebug()) {
              PUSH_EXIT_FRAME();
              if (!script->initScriptCounts(cx)) {
                goto error;
              }
            }
          }
          COUNT_COVERAGE_MAIN();
        }

        // Everything is switched to callee context now -- dispatch!
        DISPATCH();
      } while (0);

      // Slow path: use the IC!
      icregs.icVals[0] = argc;
      icregs.extraArgs = 2 + constructing;
      icregs.spreadCall = false;
      INVOKE_IC(Call);
      POPN(argc + 2 + constructing);
      PUSH(StackVal(Value::fromRawBits(icregs.icResult)));
      END_OP(Call);
    }

    CASE(SpreadCall)
    CASE(SpreadEval)
    CASE(StrictSpreadEval) {
      static_assert(JSOpLength_SpreadCall == JSOpLength_SpreadEval);
      static_assert(JSOpLength_SpreadCall == JSOpLength_StrictSpreadEval);
      icregs.icVals[0] = 1;
      icregs.extraArgs = 2;
      icregs.spreadCall = true;
      INVOKE_IC(Call);
      POPN(3);
      PUSH(StackVal(Value::fromRawBits(icregs.icResult)));
      END_OP(SpreadCall);
    }

    CASE(SpreadSuperCall)
    CASE(SpreadNew) {
      static_assert(JSOpLength_SpreadSuperCall == JSOpLength_SpreadNew);
      icregs.icVals[0] = 1;
      icregs.extraArgs = 3;
      icregs.spreadCall = true;
      INVOKE_IC(Call);
      POPN(4);
      PUSH(StackVal(Value::fromRawBits(icregs.icResult)));
      END_OP(SpreadSuperCall);
    }

    CASE(OptimizeSpreadCall) {
      IC_POP_ARG(0);
      INVOKE_IC(OptimizeSpreadCall);
      IC_PUSH_RESULT();
      END_OP(OptimizeSpreadCall);
    }

    CASE(OptimizeGetIterator) {
      IC_POP_ARG(0);
      INVOKE_IC(OptimizeGetIterator);
      IC_PUSH_RESULT();
      END_OP(OptimizeGetIterator);
    }

    CASE(ImplicitThis) {
      {
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        ReservedRooted<PropertyName*> name0(&state.name0, script->getName(pc));
        PUSH_EXIT_FRAME();
        if (!ImplicitThisOperation(cx, obj0, name0, &state.res)) {
          goto error;
        }
      }
      PUSH(StackVal(state.res));
      state.res.setUndefined();
      END_OP(ImplicitThis);
    }

    CASE(CallSiteObj) {
      JSObject* cso = script->getObject(pc);
      MOZ_ASSERT(!cso->as<ArrayObject>().isExtensible());
      MOZ_ASSERT(cso->as<ArrayObject>().containsPure(
          frameMgr.cxForLocalUseOnly()->names().raw));
      PUSH(StackVal(ObjectValue(*cso)));
      END_OP(CallSiteObj);
    }

    CASE(IsConstructing) {
      PUSH(StackVal(MagicValue(JS_IS_CONSTRUCTING)));
      END_OP(IsConstructing);
    }

    CASE(SuperFun) {
      JSObject* superEnvFunc = &POP().asValue().toObject();
      JSObject* superFun = SuperFunOperation(superEnvFunc);
      PUSH(StackVal(ObjectOrNullValue(superFun)));
      END_OP(SuperFun);
    }

    CASE(CheckThis) {
      if (sp[0].asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx));
        goto error;
      }
      END_OP(CheckThis);
    }

    CASE(CheckThisReinit) {
      if (!sp[0].asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ThrowInitializedThis(cx));
        goto error;
      }
      END_OP(CheckThisReinit);
    }

    CASE(Generator) {
      JSObject* generator;
      {
        PUSH_EXIT_FRAME();
        generator = CreateGeneratorFromFrame(cx, frame);
        if (!generator) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*generator)));
      END_OP(Generator);
    }

    CASE(InitialYield) {
      // gen => rval, gen, resumeKind
      ReservedRooted<JSObject*> obj0(&state.obj0, &sp[0].asValue().toObject());
      uint32_t frameSize = stack.frameSize(sp, frame);
      {
        PUSH_EXIT_FRAME();
        if (!NormalSuspend(cx, obj0, frame, frameSize, pc)) {
          goto error;
        }
      }
      frame->setReturnValue(sp[0].asValue());
      goto do_return;
    }

    CASE(Await)
    CASE(Yield) {
      // rval1, gen => rval2, gen, resumeKind
      ReservedRooted<JSObject*> obj0(&state.obj0, &POP().asValue().toObject());
      uint32_t frameSize = stack.frameSize(sp, frame);
      {
        PUSH_EXIT_FRAME();
        if (!NormalSuspend(cx, obj0, frame, frameSize, pc)) {
          goto error;
        }
      }
      frame->setReturnValue(sp[0].asValue());
      goto do_return;
    }

    CASE(FinalYieldRval) {
      // gen =>
      ReservedRooted<JSObject*> obj0(&state.obj0, &POP().asValue().toObject());
      {
        PUSH_EXIT_FRAME();
        if (!FinalSuspend(cx, obj0, pc)) {
          goto error;
        }
      }
      goto do_return;
    }

    CASE(IsGenClosing) {
      bool result = sp[0].asValue() == MagicValue(JS_GENERATOR_CLOSING);
      PUSH(StackVal(BooleanValue(result)));
      END_OP(IsGenClosing);
    }

    CASE(AsyncAwait) {
      // value, gen => promise
      JSObject* promise;
      {
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &POP().asValue().toObject());   // gen
        ReservedRooted<Value> value0(&state.value0, POP().asValue());  // value
        PUSH_EXIT_FRAME();
        promise = AsyncFunctionAwait(
            cx, obj0.as<AsyncFunctionGeneratorObject>(), value0);
        if (!promise) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*promise)));
      END_OP(AsyncAwait);
    }

    CASE(AsyncResolve) {
      // value, gen => promise
      JSObject* promise;
      {
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &POP().asValue().toObject());  // gen
        ReservedRooted<Value> value0(&state.value0,
                                     POP().asValue());  // value
        PUSH_EXIT_FRAME();
        promise = AsyncFunctionResolve(
            cx, obj0.as<AsyncFunctionGeneratorObject>(), value0);
        if (!promise) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*promise)));
      END_OP(AsyncResolve);
    }

    CASE(AsyncReject) {
      // reason, gen => promise
      JSObject* promise;
      {
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &POP().asValue().toObject());  // gen
        ReservedRooted<Value> value0(&state.value0,
                                     POP().asValue());  // stack
        ReservedRooted<Value> value1(&state.value1,
                                     POP().asValue());  // reason
        PUSH_EXIT_FRAME();
        promise = AsyncFunctionReject(
            cx, obj0.as<AsyncFunctionGeneratorObject>(), value1, value0);
        if (!promise) {
          goto error;
        }
      }
      PUSH(StackVal(ObjectValue(*promise)));
      END_OP(AsyncReject);
    }

    CASE(CanSkipAwait) {
      // value => value, can_skip
      bool result = false;
      {
        ReservedRooted<Value> value0(&state.value0, sp[0].asValue());
        PUSH_EXIT_FRAME();
        if (!CanSkipAwait(cx, value0, &result)) {
          goto error;
        }
      }
      PUSH(StackVal(BooleanValue(result)));
      END_OP(CanSkipAwait);
    }

    CASE(MaybeExtractAwaitValue) {
      // value, can_skip => value_or_resolved, can_skip
      {
        Value can_skip = POP().asValue();
        ReservedRooted<Value> value0(&state.value0, POP().asValue());  // value
        if (can_skip.toBoolean()) {
          PUSH_EXIT_FRAME();
          if (!ExtractAwaitValue(cx, value0, &value0)) {
            goto error;
          }
        }
        PUSH(StackVal(value0));
        PUSH(StackVal(can_skip));
      }
      END_OP(MaybeExtractAwaitValue);
    }

    CASE(ResumeKind) {
      GeneratorResumeKind resumeKind = ResumeKindFromPC(pc);
      PUSH(StackVal(Int32Value(int32_t(resumeKind))));
      END_OP(ResumeKind);
    }

    CASE(CheckResumeKind) {
      // rval, gen, resumeKind => rval
      {
        GeneratorResumeKind resumeKind =
            IntToResumeKind(POP().asValue().toInt32());
        ReservedRooted<JSObject*> obj0(&state.obj0,
                                       &POP().asValue().toObject());   // gen
        ReservedRooted<Value> value0(&state.value0, sp[0].asValue());  // rval
        if (resumeKind != GeneratorResumeKind::Next) {
          PUSH_EXIT_FRAME();
          MOZ_ALWAYS_FALSE(GeneratorThrowOrReturn(
              cx, frame, obj0.as<AbstractGeneratorObject>(), value0,
              resumeKind));
          goto error;
        }
      }
      END_OP(CheckResumeKind);
    }

    CASE(Resume) {
      Value gen = sp[2].asValue();
      Value* callerSP = reinterpret_cast<Value*>(sp);
      {
        ReservedRooted<Value> value0(&state.value0);
        ReservedRooted<JSObject*> obj0(&state.obj0, &gen.toObject());
        {
          PUSH_EXIT_FRAME();
          TRACE_PRINTF("Going to C++ interp for Resume\n");
          if (!InterpretResume(cx, obj0, callerSP, &value0)) {
            goto error;
          }
        }
        POPN(2);
        sp[0] = StackVal(value0);
      }
      END_OP(Resume);
    }

    CASE(JumpTarget) {
      int32_t icIndex = GET_INT32(pc);
      frame->interpreterICEntry() = frame->icScript()->icEntries() + icIndex;
      COUNT_COVERAGE_PC(pc);
      END_OP(JumpTarget);
    }
    CASE(LoopHead) {
      int32_t icIndex = GET_INT32(pc);
      frame->interpreterICEntry() = frame->icScript()->icEntries() + icIndex;
#ifndef __wasi__
      if (frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
        PUSH_EXIT_FRAME();
        if (!InterruptCheck(cx)) {
          goto error;
        }
      }
#endif
      COUNT_COVERAGE_PC(pc);
      END_OP(LoopHead);
    }
    CASE(AfterYield) {
      int32_t icIndex = GET_INT32(pc);
      frame->interpreterICEntry() = frame->icScript()->icEntries() + icIndex;
      if (script->isDebuggee()) {
        TRACE_PRINTF("doing DebugAfterYield\n");
        PUSH_EXIT_FRAME();
        if (DebugAPI::hasAnyBreakpointsOrStepMode(script) &&
            !HandleDebugTrap(cx, frame, pc)) {
          TRACE_PRINTF("HandleDebugTrap returned error\n");
          goto error;
        }
        if (!DebugAfterYield(cx, frame)) {
          TRACE_PRINTF("DebugAfterYield returned error\n");
          goto error;
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
      if (!sp[0].asValue().isNullOrUndefined()) {
        ADVANCE(GET_JUMP_OFFSET(pc));
        DISPATCH();
      } else {
        END_OP(Coalesce);
      }
    }

    CASE(Case) {
      bool cond = POP().asValue().toBoolean();
      if (cond) {
        POP();
        ADVANCE(GET_JUMP_OFFSET(pc));
        DISPATCH();
      } else {
        END_OP(Case);
      }
    }

    CASE(Default) {
      POP();
      ADVANCE(GET_JUMP_OFFSET(pc));
      DISPATCH();
    }

    CASE(TableSwitch) {
      int32_t len = GET_JUMP_OFFSET(pc);
      int32_t low = GET_JUMP_OFFSET(pc + 1 * JUMP_OFFSET_LEN);
      int32_t high = GET_JUMP_OFFSET(pc + 2 * JUMP_OFFSET_LEN);
      Value v = POP().asValue();
      int32_t i = 0;
      if (v.isInt32()) {
        i = v.toInt32();
      } else if (!v.isDouble() ||
                 !mozilla::NumberEqualsInt32(v.toDouble(), &i)) {
        ADVANCE(len);
        DISPATCH();
      }

      i = uint32_t(i) - uint32_t(low);
      if ((uint32_t(i) < uint32_t(high - low + 1))) {
        len = script->tableSwitchCaseOffset(pc, uint32_t(i)) -
              script->pcToOffset(pc);
      }
      ADVANCE(len);
      DISPATCH();
    }

    CASE(Return) {
      frame->setReturnValue(POP().asValue());
      goto do_return;
    }

    CASE(GetRval) {
      PUSH(StackVal(frame->returnValue()));
      END_OP(GetRval);
    }

    CASE(SetRval) {
      frame->setReturnValue(POP().asValue());
      END_OP(SetRval);
    }

  do_return:
    CASE(RetRval) {
      bool ok = true;
      if (frame->isDebuggee() && !from_unwind) {
        TRACE_PRINTF("doing DebugEpilogueOnBaselineReturn\n");
        PUSH_EXIT_FRAME();
        ok = DebugEpilogueOnBaselineReturn(cx, frame, pc);
      }
      from_unwind = false;

      uint32_t argc = frame->numActualArgs();
      sp = stack.popFrame();

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
        sp = stack.popFrame();
        // Pop fake return address and descriptor.
        POPNNATIVE(2);

        // Set PC, frame, and current script.
        frame = reinterpret_cast<BaselineFrame*>(
            reinterpret_cast<uintptr_t>(stack.fp) - BaselineFrame::Size());
        TRACE_PRINTF(" sp -> %p, fp -> %p, frame -> %p\n", sp, stack.fp, frame);
        frameMgr.switchToFrame(frame);
        pc = frame->interpreterPC();
        script.set(frame->script());

        // Adjust caller's stack to complete the call op that PC still points to
        // in that frame (pop args, push return value).
        JSOp op = JSOp(*pc);
        bool constructing = (op == JSOp::New || op == JSOp::NewContent ||
                             op == JSOp::SuperCall);
        // Fix-up return value; EnterJit would do this if we hadn't bypassed it.
        if (constructing && ret.isPrimitive()) {
          ret = sp[argc + constructing].asValue();
          TRACE_PRINTF("updated ret = %" PRIx64 "\n", ret.asRawBits());
        }
        // Pop args -- this is 1 more than how many are pushed in the
        // `totalArgs` count during the call fastpath because it includes
        // the callee.
        POPN(argc + 2 + constructing);
        // Push return value.
        PUSH(StackVal(ret));

        if (!ok) {
          goto error;
        }

        // Advance past call instruction, and advance past IC.
        NEXT_IC();
        ADVANCE(JSOpLength_Call);

        DISPATCH();
      }
    }

    CASE(CheckReturn) {
      Value thisval = POP().asValue();
      // inlined version of frame->checkReturn(thisval, result)
      // (js/src/vm/Stack.cpp).
      HandleValue retVal = frame->returnValue();
      if (retVal.isObject()) {
        PUSH(StackVal(retVal));
      } else if (!retVal.isUndefined()) {
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN,
                                          JSDVG_IGNORE_STACK, retVal, nullptr));
        goto error;
      } else if (thisval.isMagic(JS_UNINITIALIZED_LEXICAL)) {
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ThrowUninitializedThis(cx));
        goto error;
      } else {
        PUSH(StackVal(thisval));
      }
      END_OP(CheckReturn);
    }

    CASE(Throw) {
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ThrowOperation(cx, value0));
        goto error;
      }
      END_OP(Throw);
    }

    CASE(ThrowWithStack) {
      {
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        ReservedRooted<Value> value1(&state.value1, POP().asValue());
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ThrowWithStackOperation(cx, value1, value0));
        goto error;
      }
      END_OP(ThrowWithStack);
    }

    CASE(ThrowMsg) {
      {
        PUSH_EXIT_FRAME();
        MOZ_ALWAYS_FALSE(ThrowMsgOperation(cx, GET_UINT8(pc)));
        goto error;
      }
      END_OP(ThrowMsg);
    }

    CASE(ThrowSetConst) {
      {
        PUSH_EXIT_FRAME();
        ReportRuntimeLexicalError(cx, JSMSG_BAD_CONST_ASSIGN, script, pc);
        goto error;
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
          goto error;
        }
      }
      PUSH(StackVal(state.res));
      state.res.setUndefined();
      END_OP(Exception);
    }

    CASE(ExceptionAndStack) {
      {
        ReservedRooted<Value> value0(&state.value0);
        {
          PUSH_EXIT_FRAME();
          if (!cx.getCx()->getPendingExceptionStack(&value0)) {
            goto error;
          }
          if (!GetAndClearException(cx, &state.res)) {
            goto error;
          }
        }
        PUSH(StackVal(state.res));
        PUSH(StackVal(value0));
        state.res.setUndefined();
      }
      END_OP(ExceptionAndStack);
    }

    CASE(Finally) {
#ifndef __wasi__
      if (frameMgr.cxForLocalUseOnly()->hasAnyPendingInterrupt()) {
        PUSH_EXIT_FRAME();
        if (!InterruptCheck(cx)) {
          goto error;
        }
      }
#endif
      END_OP(Finally);
    }

    CASE(Uninitialized) {
      PUSH(StackVal(MagicValue(JS_UNINITIALIZED_LEXICAL)));
      END_OP(Uninitialized);
    }
    CASE(InitLexical) {
      uint32_t i = GET_LOCALNO(pc);
      frame->unaliasedLocal(i) = sp[0].asValue();
      END_OP(InitLexical);
    }

    CASE(InitAliasedLexical) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(pc);
      EnvironmentObject& obj = getEnvironmentFromCoordinate(frame, ec);
      obj.setAliasedBinding(ec, sp[0].asValue());
      END_OP(InitAliasedLexical);
    }
    CASE(CheckLexical) {
      if (sp[0].asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
        PUSH_EXIT_FRAME();
        ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script, pc);
        goto error;
      }
      END_OP(CheckLexical);
    }
    CASE(CheckAliasedLexical) {
      if (sp[0].asValue().isMagic(JS_UNINITIALIZED_LEXICAL)) {
        PUSH_EXIT_FRAME();
        ReportRuntimeLexicalError(cx, JSMSG_UNINITIALIZED_LEXICAL, script, pc);
        goto error;
      }
      END_OP(CheckAliasedLexical);
    }

    CASE(BindGName) {
      IC_SET_OBJ_ARG(
          0, &frameMgr.cxForLocalUseOnly()->global()->lexicalEnvironment());
      INVOKE_IC(BindName);
      IC_PUSH_RESULT();
      END_OP(BindGName);
    }
    CASE(BindName) {
      IC_SET_OBJ_ARG(0, frame->environmentChain());
      INVOKE_IC(BindName);
      IC_PUSH_RESULT();
      END_OP(BindName);
    }
    CASE(GetGName) {
      IC_SET_OBJ_ARG(
          0, &frameMgr.cxForLocalUseOnly()->global()->lexicalEnvironment());
      INVOKE_IC(GetName);
      IC_PUSH_RESULT();
      END_OP(GetGName);
    }
    CASE(GetName) {
      IC_SET_OBJ_ARG(0, frame->environmentChain());
      INVOKE_IC(GetName);
      IC_PUSH_RESULT();
      END_OP(GetName);
    }

    CASE(GetArg) {
      unsigned i = GET_ARGNO(pc);
      if (script->argsObjAliasesFormals()) {
        PUSH(StackVal(frame->argsObj().arg(i)));
      } else {
        PUSH(StackVal(frame->unaliasedFormal(i)));
      }
      END_OP(GetArg);
    }

    CASE(GetFrameArg) {
      uint32_t i = GET_ARGNO(pc);
      PUSH(StackVal(frame->unaliasedFormal(i, DONT_CHECK_ALIASING)));
      END_OP(GetFrameArg);
    }

    CASE(GetLocal) {
      uint32_t i = GET_LOCALNO(pc);
      TRACE_PRINTF(" -> local: %d\n", int(i));
      PUSH(StackVal(frame->unaliasedLocal(i)));
      END_OP(GetLocal);
    }

    CASE(ArgumentsLength) {
      PUSH(StackVal(Int32Value(frame->numActualArgs())));
      END_OP(ArgumentsLength);
    }

    CASE(GetActualArg) {
      MOZ_ASSERT(!script->needsArgsObj());
      uint32_t index = sp[0].asValue().toInt32();
      sp[0] = StackVal(frame->unaliasedActual(index));
      END_OP(GetActualArg);
    }

    CASE(GetAliasedVar)
    CASE(GetAliasedDebugVar) {
      static_assert(JSOpLength_GetAliasedVar == JSOpLength_GetAliasedDebugVar);
      EnvironmentCoordinate ec = EnvironmentCoordinate(pc);
      EnvironmentObject& obj = getEnvironmentFromCoordinate(frame, ec);
      PUSH(StackVal(obj.aliasedBinding(ec)));
      END_OP(GetAliasedVar);
    }

    CASE(GetImport) {
      {
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        ReservedRooted<Value> value0(&state.value0);
        {
          PUSH_EXIT_FRAME();
          if (!GetImportOperation(cx, obj0, script, pc, &value0)) {
            goto error;
          }
        }
        PUSH(StackVal(value0));
      }
      END_OP(GetImport);
    }

    CASE(GetIntrinsic) {
      INVOKE_IC(GetIntrinsic);
      IC_PUSH_RESULT();
      END_OP(GetIntrinsic);
    }

    CASE(Callee) {
      PUSH(StackVal(frame->calleev()));
      END_OP(Callee);
    }

    CASE(EnvCallee) {
      uint8_t numHops = GET_UINT8(pc);
      JSObject* env = &frame->environmentChain()->as<EnvironmentObject>();
      for (unsigned i = 0; i < numHops; i++) {
        env = &env->as<EnvironmentObject>().enclosingEnvironment();
      }
      PUSH(StackVal(ObjectValue(env->as<CallObject>().callee())));
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
      PUSH(StackVal(icregs.icVals[1]));
      INVOKE_IC(SetProp);
      END_OP(SetProp);
    }

    CASE(InitProp)
    CASE(InitHiddenProp)
    CASE(InitLockedProp) {
      static_assert(JSOpLength_InitProp == JSOpLength_InitHiddenProp);
      static_assert(JSOpLength_InitProp == JSOpLength_InitLockedProp);
      IC_POP_ARG(1);
      IC_SET_ARG_FROM_STACK(0, 0);
      INVOKE_IC(SetProp);
      END_OP(InitProp);
    }
    CASE(InitGLexical) {
      IC_SET_ARG_FROM_STACK(1, 0);
      IC_SET_OBJ_ARG(
          0, &frameMgr.cxForLocalUseOnly()->global()->lexicalEnvironment());
      INVOKE_IC(SetProp);
      END_OP(InitGLexical);
    }

    CASE(SetArg) {
      unsigned i = GET_ARGNO(pc);
      if (script->argsObjAliasesFormals()) {
        frame->argsObj().setArg(i, sp[0].asValue());
      } else {
        frame->unaliasedFormal(i) = sp[0].asValue();
      }
      END_OP(SetArg);
    }

    CASE(SetLocal) {
      uint32_t i = GET_LOCALNO(pc);
      TRACE_PRINTF(" -> local: %d\n", int(i));
      frame->unaliasedLocal(i) = sp[0].asValue();
      END_OP(SetLocal);
    }

    CASE(SetAliasedVar) {
      EnvironmentCoordinate ec = EnvironmentCoordinate(pc);
      EnvironmentObject& obj = getEnvironmentFromCoordinate(frame, ec);
      MOZ_ASSERT(!IsUninitializedLexical(obj.aliasedBinding(ec)));
      obj.setAliasedBinding(ec, sp[0].asValue());
      END_OP(SetAliasedVar);
    }

    CASE(SetIntrinsic) {
      {
        ReservedRooted<Value> value0(&state.value0, sp[0].asValue());
        {
          PUSH_EXIT_FRAME();
          if (!SetIntrinsicOperation(cx, script, pc, value0)) {
            goto error;
          }
        }
      }
      END_OP(SetIntrinsic);
    }

    CASE(PushLexicalEnv) {
      {
        ReservedRooted<Scope*> scope0(&state.scope0, script->getScope(pc));
        {
          PUSH_EXIT_FRAME();
          if (!frame->pushLexicalEnvironment(cx, scope0.as<LexicalScope>())) {
            goto error;
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
          goto error;
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
          goto error;
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
            goto error;
          }
        } else {
          if (!frame->recreateLexicalEnvironment<false>(cx)) {
            goto error;
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
            goto error;
          }
        } else {
          if (!frame->freshenLexicalEnvironment<false>(cx)) {
            goto error;
          }
        }
      }
      END_OP(FreshenLexicalEnv);
    }
    CASE(PushClassBodyEnv) {
      {
        ReservedRooted<Scope*> scope0(&state.scope0, script->getScope(pc));
        PUSH_EXIT_FRAME();
        if (!frame->pushClassBodyEnvironment(cx, scope0.as<ClassBodyScope>())) {
          goto error;
        }
      }
      END_OP(PushClassBodyEnv);
    }
    CASE(PushVarEnv) {
      {
        ReservedRooted<Scope*> scope0(&state.scope0, script->getScope(pc));
        PUSH_EXIT_FRAME();
        if (!frame->pushVarEnvironment(cx, scope0)) {
          goto error;
        }
      }
      END_OP(PushVarEnv);
    }
    CASE(EnterWith) {
      {
        ReservedRooted<Scope*> scope0(&state.scope0, script->getScope(pc));
        ReservedRooted<Value> value0(&state.value0, POP().asValue());
        PUSH_EXIT_FRAME();
        if (!EnterWithOperation(cx, frame, value0, scope0.as<WithScope>())) {
          goto error;
        }
      }
      END_OP(EnterWith);
    }
    CASE(LeaveWith) {
      frame->popOffEnvironmentChain<WithEnvironmentObject>();
      END_OP(LeaveWith);
    }
    CASE(BindVar) {
      JSObject* varObj;
      {
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        PUSH_EXIT_FRAME();
        varObj = BindVarOperation(cx, obj0);
      }
      PUSH(StackVal(ObjectValue(*varObj)));
      END_OP(BindVar);
    }

    CASE(GlobalOrEvalDeclInstantiation) {
      GCThingIndex lastFun = GET_GCTHING_INDEX(pc);
      {
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        PUSH_EXIT_FRAME();
        if (!GlobalOrEvalDeclInstantiation(cx, obj0, script, lastFun)) {
          goto error;
        }
      }
      END_OP(GlobalOrEvalDeclInstantiation);
    }

    CASE(DelName) {
      {
        ReservedRooted<PropertyName*> name0(&state.name0, script->getName(pc));
        ReservedRooted<JSObject*> obj0(&state.obj0, frame->environmentChain());
        PUSH_EXIT_FRAME();
        if (!DeleteNameOperation(cx, name0, obj0, &state.res)) {
          goto error;
        }
      }
      PUSH(StackVal(state.res));
      state.res.setUndefined();
      END_OP(DelName);
    }

    CASE(Arguments) {
      {
        PUSH_EXIT_FRAME();
        if (!NewArgumentsObject(cx, frame, &state.res)) {
          goto error;
        }
      }
      PUSH(StackVal(state.res));
      state.res.setUndefined();
      END_OP(Arguments);
    }

    CASE(Rest) {
      INVOKE_IC(Rest);
      IC_PUSH_RESULT();
      END_OP(Rest);
    }

    CASE(FunctionThis) {
      {
        PUSH_EXIT_FRAME();
        if (!js::GetFunctionThis(cx, frame, &state.res)) {
          goto error;
        }
      }
      PUSH(StackVal(state.res));
      state.res.setUndefined();
      END_OP(FunctionThis);
    }

    CASE(Pop) {
      POP();
      END_OP(Pop);
    }
    CASE(PopN) {
      uint32_t n = GET_UINT16(pc);
      POPN(n);
      END_OP(PopN);
    }
    CASE(Dup) {
      StackVal value = sp[0];
      PUSH(value);
      END_OP(Dup);
    }
    CASE(Dup2) {
      StackVal value1 = sp[0];
      StackVal value2 = sp[1];
      PUSH(value2);
      PUSH(value1);
      END_OP(Dup2);
    }
    CASE(DupAt) {
      unsigned i = GET_UINT24(pc);
      StackVal value = sp[i];
      PUSH(value);
      END_OP(DupAt);
    }
    CASE(Swap) {
      std::swap(sp[0], sp[1]);
      END_OP(Swap);
    }
    CASE(Pick) {
      unsigned i = GET_UINT8(pc);
      StackVal tmp = sp[i];
      memmove(&sp[1], &sp[0], sizeof(StackVal) * i);
      sp[0] = tmp;
      END_OP(Pick);
    }
    CASE(Unpick) {
      unsigned i = GET_UINT8(pc);
      StackVal tmp = sp[0];
      memmove(&sp[0], &sp[1], sizeof(StackVal) * i);
      sp[i] = tmp;
      END_OP(Unpick);
    }
    CASE(DebugCheckSelfHosted) {
      HandleValue val = Stack::handle(&sp[0]);
      {
        PUSH_EXIT_FRAME();
        if (!Debug_CheckSelfHosted(cx, val)) {
          goto error;
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
          goto error;
        }
      }
      END_OP(Debugger);
    }

  label_default:
    MOZ_CRASH("Bad opcode");
  }

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
        stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        stack.unwindingSP = reinterpret_cast<StackVal*>(rfe.stackPointer);
        goto unwind_error;
      case ExceptionResumeKind::Catch:
        pc = frame->interpreterPC();
        stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        stack.unwindingSP = reinterpret_cast<StackVal*>(rfe.stackPointer);
        TRACE_PRINTF(" -> catch to pc %p\n", pc);
        goto unwind;
      case ExceptionResumeKind::Finally:
        pc = frame->interpreterPC();
        stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        sp = reinterpret_cast<StackVal*>(rfe.stackPointer);
        TRACE_PRINTF(" -> finally to pc %p\n", pc);
        PUSH(StackVal(rfe.exception));
        PUSH(StackVal(rfe.exceptionStack));
        PUSH(StackVal(BooleanValue(true)));
        stack.unwindingSP = sp;
        goto unwind;
      case ExceptionResumeKind::ForcedReturnBaseline:
        pc = frame->interpreterPC();
        stack.fp = reinterpret_cast<StackVal*>(rfe.framePointer);
        stack.unwindingSP = reinterpret_cast<StackVal*>(rfe.stackPointer);
        TRACE_PRINTF(" -> forced return\n");
        goto unwind_ret;
      case ExceptionResumeKind::ForcedReturnIon:
        MOZ_CRASH(
            "Unexpected ForcedReturnIon exception-resume kind in Portable "
            "Baseline");
      case ExceptionResumeKind::Bailout:
        MOZ_CRASH(
            "Unexpected Bailout exception-resume kind in Portable Baseline");
      case ExceptionResumeKind::Wasm:
        MOZ_CRASH("Unexpected Wasm exception-resume kind in Portable Baseline");
      case ExceptionResumeKind::WasmCatch:
        MOZ_CRASH(
            "Unexpected WasmCatch exception-resume kind in Portable "
            "Baseline");
    }
  }

  DISPATCH();

unwind:
  TRACE_PRINTF("unwind: fp = %p entryFrame = %p\n", stack.fp, entryFrame);
  if (reinterpret_cast<uintptr_t>(stack.fp) >
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    TRACE_PRINTF(" -> returning\n");
    return PBIResult::Unwind;
  }
  sp = stack.unwindingSP;
  frame = reinterpret_cast<BaselineFrame*>(
      reinterpret_cast<uintptr_t>(stack.fp) - BaselineFrame::Size());
  TRACE_PRINTF(" -> setting sp to %p, frame to %p\n", sp, frame);
  frameMgr.switchToFrame(frame);
  pc = frame->interpreterPC();
  script.set(frame->script());
  DISPATCH();
unwind_error:
  TRACE_PRINTF("unwind_error: fp = %p entryFrame = %p\n", stack.fp, entryFrame);
  if (reinterpret_cast<uintptr_t>(stack.fp) >
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    return PBIResult::UnwindError;
  }
  if (reinterpret_cast<uintptr_t>(stack.fp) ==
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    return PBIResult::Error;
  }
  sp = stack.unwindingSP;
  frame = reinterpret_cast<BaselineFrame*>(
      reinterpret_cast<uintptr_t>(stack.fp) - BaselineFrame::Size());
  TRACE_PRINTF(" -> setting sp to %p, frame to %p\n", sp, frame);
  frameMgr.switchToFrame(frame);
  pc = frame->interpreterPC();
  script.set(frame->script());
  goto error;
unwind_ret:
  TRACE_PRINTF("unwind_ret: fp = %p entryFrame = %p\n", stack.fp, entryFrame);
  if (reinterpret_cast<uintptr_t>(stack.fp) >
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    return PBIResult::UnwindRet;
  }
  if (reinterpret_cast<uintptr_t>(stack.fp) ==
      reinterpret_cast<uintptr_t>(entryFrame) + BaselineFrame::Size()) {
    *ret = frame->returnValue();
    return PBIResult::Ok;
  }
  sp = stack.unwindingSP;
  frame = reinterpret_cast<BaselineFrame*>(
      reinterpret_cast<uintptr_t>(stack.fp) - BaselineFrame::Size());
  TRACE_PRINTF(" -> setting sp to %p, frame to %p\n", sp, frame);
  frameMgr.switchToFrame(frame);
  pc = frame->interpreterPC();
  script.set(frame->script());
  from_unwind = true;
  goto do_return;

#ifndef __wasi__
debug: {
  TRACE_PRINTF("hit debug point\n");
  PUSH_EXIT_FRAME();
  if (!HandleDebugTrap(cx, frame, pc)) {
    TRACE_PRINTF("HandleDebugTrap returned error\n");
    goto error;
  }
  pc = frame->interpreterPC();
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

  switch (PortableBaselineInterpret(cx, state, stack, sp, envChain, result)) {
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
