/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitFrames_h
#define jit_JitFrames_h

#include "mozilla/Assertions.h"

#include <stddef.h>
#include <stdint.h>

#include "jit/CalleeToken.h"
#include "jit/MachineState.h"
#include "jit/Registers.h"
#include "js/Id.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

namespace js {

namespace wasm {
class Instance;
}

namespace jit {

enum class FrameType;
enum class VMFunctionId;
class IonScript;
class JitActivation;
class JitFrameLayout;
struct SafepointSlotEntry;
struct VMFunctionData;

// [SMDOC] JIT Frame Layout
//
// Frame Headers:
//
// In between every two frames lies a small header describing both frames. This
// header, minimally, contains a returnAddress word and a descriptor word (See
// CommonFrameLayout). The descriptor describes the type of the older (caller)
// frame, whereas the returnAddress describes the address the newer (callee)
// frame will return to. For JitFrameLayout, the descriptor also stores the
// number of arguments passed from the caller to the callee frame.
//
// Special Frames:
//
// Two special frame types exist:
// - Entry frames begin a JitActivation, and therefore there is exactly one
//   per activation of EnterJit or EnterBaseline. These reuse JitFrameLayout.
// - Exit frames are necessary to leave JIT code and enter C++, and thus,
//   C++ code will always begin iterating from the topmost exit frame.
//
// Approximate Layout:
//
// The layout of an Ion frame on the C stack is roughly:
//      argN     _
//      ...       \ - These are jsvals
//      arg0      /
//   -3 this    _/
//   -2 callee
//   -1 descriptor
//    0 returnAddress
//   .. locals ..

// [SMDOC] Frame Descriptor Layout
//
// A frame descriptor word has the following data:
//
//    high bits: [ numActualArgs |
//                 has-cached-saved-frame bit |
//    low bits:    frame type ]
//
// * numActualArgs: for JitFrameLayout, the number of arguments passed by the
//   caller.
// * Has-cache-saved-frame bit: Used to power the LiveSavedFrameCache
//   optimization. See the comment in Activation.h
// * Frame Type: BaselineJS, Exit, etc. (jit::FrameType)
//

static const uintptr_t FRAMETYPE_BITS = 4;
static const uintptr_t FRAMETYPE_MASK = (1 << FRAMETYPE_BITS) - 1;
static const uintptr_t HASCACHEDSAVEDFRAME_BIT = 1 << FRAMETYPE_BITS;
static const uintptr_t NUMACTUALARGS_SHIFT =
    FRAMETYPE_BITS + 1 /* HASCACHEDSAVEDFRAME_BIT */;

struct BaselineBailoutInfo;

enum class ExceptionResumeKind : int32_t {
  // There is no exception handler in this activation.
  // Return from the entry frame.
  EntryFrame,

  // The exception was caught in baseline.
  // Restore state and jump to the catch block.
  Catch,

  // A finally block must be executed in baseline.
  // Stash the exception on the stack and jump to the finally block.
  Finally,

  // We are forcing an early return with a specific return value.
  // This is used by the debugger and when closing generators.
  // Immediately return from the current frame with the given value.
  ForcedReturnBaseline,
  ForcedReturnIon,

  // This frame is currently executing in Ion, but we must bail out
  // to baseline before handling the exception.
  // Jump to the bailout tail stub.
  Bailout,

  // The innermost frame was a wasm frame.
  // Return to the wasm entry frame.
  Wasm,

  // The exception was caught by a wasm catch handler.
  // Restore state and jump to it.
  WasmCatch
};

// Data needed to recover from an exception.
struct ResumeFromException {
  uint8_t* framePointer;
  uint8_t* stackPointer;
  uint8_t* target;
  ExceptionResumeKind kind;
  wasm::Instance* instance;

  // Value to push when resuming into a |finally| block.
  // Also used by Wasm to send the exception object to the throw stub.
  JS::Value exception;

  // Exception stack to push when resuming into a |finally| block.
  JS::Value exceptionStack;

  BaselineBailoutInfo* bailoutInfo;

  static size_t offsetOfFramePointer() {
    return offsetof(ResumeFromException, framePointer);
  }
  static size_t offsetOfStackPointer() {
    return offsetof(ResumeFromException, stackPointer);
  }
  static size_t offsetOfTarget() {
    return offsetof(ResumeFromException, target);
  }
  static size_t offsetOfKind() { return offsetof(ResumeFromException, kind); }
  static size_t offsetOfInstance() {
    return offsetof(ResumeFromException, instance);
  }
  static size_t offsetOfException() {
    return offsetof(ResumeFromException, exception);
  }
  static size_t offsetOfExceptionStack() {
    return offsetof(ResumeFromException, exceptionStack);
  }
  static size_t offsetOfBailoutInfo() {
    return offsetof(ResumeFromException, bailoutInfo);
  }
};

#if defined(JS_CODEGEN_ARM64)
static_assert(sizeof(ResumeFromException) % 16 == 0,
              "ResumeFromException should be aligned");
#endif

void HandleException(ResumeFromException* rfe);

void EnsureUnwoundJitExitFrame(JitActivation* act, JitFrameLayout* frame);

void TraceJitActivations(JSContext* cx, JSTracer* trc);

// Trace weak pointers in baseline stubs in activations for zones that are
// currently being swept.
void TraceWeakJitActivationsInSweepingZones(JSContext* cx, JSTracer* trc);

void UpdateJitActivationsForMinorGC(JSRuntime* rt);
void UpdateJitActivationsForCompactingGC(JSRuntime* rt);

static inline uint32_t MakeFrameDescriptor(FrameType type) {
  return uint32_t(type);
}

// For JitFrameLayout, the descriptor also stores the number of arguments passed
// by the caller. Note that |type| is the type of the *older* frame and |argc|
// is the number of arguments passed to the *newer* frame.
static inline uint32_t MakeFrameDescriptorForJitCall(FrameType type,
                                                     uint32_t argc) {
  uint32_t descriptor = (argc << NUMACTUALARGS_SHIFT) | uint32_t(type);
  MOZ_ASSERT((descriptor >> NUMACTUALARGS_SHIFT) == argc,
             "argc must fit in descriptor");
  return descriptor;
}

// Returns the JSScript associated with the topmost JIT frame.
JSScript* GetTopJitJSScript(JSContext* cx);

#if defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_ARM64)
uint8_t* alignDoubleSpill(uint8_t* pointer);
#else
inline uint8_t* alignDoubleSpill(uint8_t* pointer) {
  // This is a no-op on most platforms.
  return pointer;
}
#endif

// Layout of the frame prefix. This assumes the stack architecture grows down.
// If this is ever not the case, we'll have to refactor.
class CommonFrameLayout {
  uint8_t* callerFramePtr_;
  uint8_t* returnAddress_;
  uintptr_t descriptor_;

 public:
  static constexpr size_t offsetOfDescriptor() {
    return offsetof(CommonFrameLayout, descriptor_);
  }
  uintptr_t descriptor() const { return descriptor_; }
  static constexpr size_t offsetOfReturnAddress() {
    return offsetof(CommonFrameLayout, returnAddress_);
  }
  FrameType prevType() const { return FrameType(descriptor_ & FRAMETYPE_MASK); }
  void changePrevType(FrameType type) {
    descriptor_ &= ~FRAMETYPE_MASK;
    descriptor_ |= uintptr_t(type);
  }
  bool hasCachedSavedFrame() const {
    return descriptor_ & HASCACHEDSAVEDFRAME_BIT;
  }
  void setHasCachedSavedFrame() { descriptor_ |= HASCACHEDSAVEDFRAME_BIT; }
  void clearHasCachedSavedFrame() { descriptor_ &= ~HASCACHEDSAVEDFRAME_BIT; }
  uint8_t* returnAddress() const { return returnAddress_; }
  void setReturnAddress(uint8_t* addr) { returnAddress_ = addr; }

  uint8_t* callerFramePtr() const { return callerFramePtr_; }
  static constexpr size_t offsetOfCallerFramePtr() {
    return offsetof(CommonFrameLayout, callerFramePtr_);
  }
  static constexpr size_t bytesPoppedAfterCall() {
    // The return address and frame pointer are popped by the callee/call.
    return 2 * sizeof(void*);
  }
};

class JitFrameLayout : public CommonFrameLayout {
  CalleeToken calleeToken_;

 public:
  CalleeToken calleeToken() const { return calleeToken_; }
  void replaceCalleeToken(CalleeToken calleeToken) {
    calleeToken_ = calleeToken;
  }

  static constexpr size_t offsetOfCalleeToken() {
    return offsetof(JitFrameLayout, calleeToken_);
  }
  static constexpr size_t offsetOfThis() { return sizeof(JitFrameLayout); }
  static constexpr size_t offsetOfActualArgs() {
    return offsetOfThis() + sizeof(JS::Value);
  }
  static constexpr size_t offsetOfActualArg(size_t arg) {
    return offsetOfActualArgs() + arg * sizeof(JS::Value);
  }

  JS::Value& thisv() {
    MOZ_ASSERT(CalleeTokenIsFunction(calleeToken()));
    return thisAndActualArgs()[0];
  }
  JS::Value* thisAndActualArgs() {
    MOZ_ASSERT(CalleeTokenIsFunction(calleeToken()));
    return (JS::Value*)(this + 1);
  }
  JS::Value* actualArgs() { return thisAndActualArgs() + 1; }
  uintptr_t numActualArgs() const {
    return descriptor() >> NUMACTUALARGS_SHIFT;
  }

  // Computes a reference to a stack or argument slot, where a slot is a
  // distance from the base frame pointer, as would be used for LStackSlot
  // or LArgument.
  uintptr_t* slotRef(SafepointSlotEntry where);

  static inline size_t Size() { return sizeof(JitFrameLayout); }
};

class BaselineInterpreterEntryFrameLayout : public JitFrameLayout {
 public:
  static inline size_t Size() {
    return sizeof(BaselineInterpreterEntryFrameLayout);
  }
};

class RectifierFrameLayout : public JitFrameLayout {
 public:
  static inline size_t Size() { return sizeof(RectifierFrameLayout); }
};

class TrampolineNativeFrameLayout : public JitFrameLayout {
 public:
  static inline size_t Size() { return sizeof(TrampolineNativeFrameLayout); }

  template <typename T>
  T* getFrameData() {
    uint8_t* raw = reinterpret_cast<uint8_t*>(this) - sizeof(T);
    return reinterpret_cast<T*>(raw);
  }
};

class WasmToJSJitFrameLayout : public JitFrameLayout {
 public:
  static inline size_t Size() { return sizeof(WasmToJSJitFrameLayout); }
};

class IonICCallFrameLayout : public CommonFrameLayout {
 protected:
  // Pointer to root the stub's JitCode.
  JitCode* stubCode_;

 public:
  static constexpr size_t LocallyTracedValueOffset = sizeof(void*);

  JitCode** stubCode() { return &stubCode_; }
  static size_t Size() { return sizeof(IonICCallFrameLayout); }

  inline Value* locallyTracedValuePtr(size_t index) {
    uint8_t* fp = reinterpret_cast<uint8_t*>(this);
    return reinterpret_cast<Value*>(fp - LocallyTracedValueOffset -
                                    index * sizeof(Value));
  }
};

enum class ExitFrameType : uint8_t {
  CallNative = 0x0,
  ConstructNative = 0x1,
  IonDOMGetter = 0x2,
  IonDOMSetter = 0x3,
  IonDOMMethod = 0x4,
  IonOOLNative = 0x5,
  IonOOLProxy = 0x6,
  WasmGenericJitEntry = 0x7,
  DirectWasmJitCall = 0x8,
  UnwoundJit = 0x9,
  InterpreterStub = 0xA,
  LazyLink = 0xB,
  Bare = 0xC,

  // This must be the last value in this enum. See ExitFooterFrame::data_.
  VMFunction = 0xD
};

// GC related data used to keep alive data surrounding the Exit frame.
class ExitFooterFrame {
  // Stores either the ExitFrameType or, for a VMFunction call,
  // `ExitFrameType::VMFunction + VMFunctionId`.
  uintptr_t data_;

#ifdef DEBUG
  void assertValidVMFunctionId() const;
#else
  void assertValidVMFunctionId() const {}
#endif

 public:
  static constexpr size_t Size() { return sizeof(ExitFooterFrame); }
  void setUnwoundJitExitFrame() {
    data_ = uintptr_t(ExitFrameType::UnwoundJit);
  }
  ExitFrameType type() const {
    if (data_ >= uintptr_t(ExitFrameType::VMFunction)) {
      return ExitFrameType::VMFunction;
    }
    return ExitFrameType(data_);
  }
  VMFunctionId functionId() const {
    MOZ_ASSERT(type() == ExitFrameType::VMFunction);
    assertValidVMFunctionId();
    return static_cast<VMFunctionId>(data_ - size_t(ExitFrameType::VMFunction));
  }

#ifdef JS_CODEGEN_MIPS32
  uint8_t* alignedForABI() {
    // See: MacroAssemblerMIPSCompat::alignStackPointer()
    uint8_t* address = reinterpret_cast<uint8_t*>(this);
    address -= sizeof(intptr_t);
    return alignDoubleSpill(address);
  }
#else
  uint8_t* alignedForABI() {
    // This is NO-OP on non-MIPS platforms.
    return reinterpret_cast<uint8_t*>(this);
  }
#endif

  // This should only be called for function()->outParam == Type_Handle
  template <typename T>
  T* outParam() {
    uint8_t* address = alignedForABI();
    return reinterpret_cast<T*>(address - sizeof(T));
  }
};

class NativeExitFrameLayout;
class IonOOLNativeExitFrameLayout;
class IonOOLProxyExitFrameLayout;
class IonDOMExitFrameLayout;

// this is the frame layout when we are exiting ion code, and about to enter
// platform ABI code
class ExitFrameLayout : public CommonFrameLayout {
  inline uint8_t* top() { return reinterpret_cast<uint8_t*>(this + 1); }

 public:
  static inline size_t Size() { return sizeof(ExitFrameLayout); }
  static inline size_t SizeWithFooter() {
    return Size() + ExitFooterFrame::Size();
  }

  inline ExitFooterFrame* footer() {
    uint8_t* sp = reinterpret_cast<uint8_t*>(this);
    return reinterpret_cast<ExitFooterFrame*>(sp - ExitFooterFrame::Size());
  }

  // argBase targets the point which precedes the exit frame. Arguments of VM
  // each wrapper are pushed before the exit frame.  This correspond exactly
  // to the value of the argBase register of the generateVMWrapper function.
  inline uint8_t* argBase() {
    MOZ_ASSERT(isWrapperExit());
    return top();
  }

  inline bool isWrapperExit() {
    return footer()->type() == ExitFrameType::VMFunction;
  }
  inline bool isBareExit() { return footer()->type() == ExitFrameType::Bare; }
  inline bool isUnwoundJitExit() {
    return footer()->type() == ExitFrameType::UnwoundJit;
  }

  // See the various exit frame layouts below.
  template <typename T>
  inline bool is() {
    return footer()->type() == T::Type();
  }
  template <typename T>
  inline T* as() {
    MOZ_ASSERT(this->is<T>());
    return reinterpret_cast<T*>(footer());
  }
};

// Cannot inherit implementation since we need to extend the top of
// ExitFrameLayout.
class NativeExitFrameLayout {
 protected:  // only to silence a clang warning about unused private fields
  ExitFooterFrame footer_;
  ExitFrameLayout exit_;
  uintptr_t argc_;

  // We need to split the Value into 2 fields of 32 bits, otherwise the C++
  // compiler may add some padding between the fields.
  uint32_t loCalleeResult_;
  uint32_t hiCalleeResult_;

 public:
  static inline size_t Size() { return sizeof(NativeExitFrameLayout); }

  static size_t offsetOfResult() {
    return offsetof(NativeExitFrameLayout, loCalleeResult_);
  }
  inline JS::Value* vp() {
    return reinterpret_cast<JS::Value*>(&loCalleeResult_);
  }
  inline uintptr_t argc() const { return argc_; }
};

class CallNativeExitFrameLayout : public NativeExitFrameLayout {
 public:
  static ExitFrameType Type() { return ExitFrameType::CallNative; }
};

class ConstructNativeExitFrameLayout : public NativeExitFrameLayout {
 public:
  static ExitFrameType Type() { return ExitFrameType::ConstructNative; }
};

template <>
inline bool ExitFrameLayout::is<NativeExitFrameLayout>() {
  return is<CallNativeExitFrameLayout>() ||
         is<ConstructNativeExitFrameLayout>();
}

class IonOOLNativeExitFrameLayout {
 protected:  // only to silence a clang warning about unused private fields
  ExitFooterFrame footer_;
  ExitFrameLayout exit_;

  // pointer to root the stub's JitCode
  JitCode* stubCode_;

  uintptr_t argc_;

  // We need to split the Value into 2 fields of 32 bits, otherwise the C++
  // compiler may add some padding between the fields.
  uint32_t loCalleeResult_;
  uint32_t hiCalleeResult_;

  // Split Value for |this| and args above.
  uint32_t loThis_;
  uint32_t hiThis_;

 public:
  static ExitFrameType Type() { return ExitFrameType::IonOOLNative; }

  static inline size_t Size(size_t argc) {
    // The frame accounts for the callee/result and |this|, so we only need
    // args.
    return sizeof(IonOOLNativeExitFrameLayout) + (argc * sizeof(JS::Value));
  }

  static size_t offsetOfResult() {
    return offsetof(IonOOLNativeExitFrameLayout, loCalleeResult_);
  }

  inline JitCode** stubCode() { return &stubCode_; }
  inline JS::Value* vp() {
    return reinterpret_cast<JS::Value*>(&loCalleeResult_);
  }
  inline JS::Value* thisp() { return reinterpret_cast<JS::Value*>(&loThis_); }
  inline uintptr_t argc() const { return argc_; }
};

// ProxyGetProperty(JSContext* cx, HandleObject proxy, HandleId id,
//                  MutableHandleValue vp)
// ProxyCallProperty(JSContext* cx, HandleObject proxy, HandleId id,
//                   MutableHandleValue vp)
// ProxySetProperty(JSContext* cx, HandleObject proxy, HandleId id,
//                  MutableHandleValue vp, bool strict)
class IonOOLProxyExitFrameLayout {
 protected:  // only to silence a clang warning about unused private fields
  ExitFooterFrame footer_;
  ExitFrameLayout exit_;

  // The proxy object.
  JSObject* proxy_;

  // id for HandleId
  jsid id_;

  // space for MutableHandleValue result
  // use two uint32_t so compiler doesn't align.
  uint32_t vp0_;
  uint32_t vp1_;

  // pointer to root the stub's JitCode
  JitCode* stubCode_;

 public:
  static ExitFrameType Type() { return ExitFrameType::IonOOLProxy; }

  static inline size_t Size() { return sizeof(IonOOLProxyExitFrameLayout); }

  static size_t offsetOfResult() {
    return offsetof(IonOOLProxyExitFrameLayout, vp0_);
  }

  inline JitCode** stubCode() { return &stubCode_; }
  inline JS::Value* vp() { return reinterpret_cast<JS::Value*>(&vp0_); }
  inline jsid* id() { return &id_; }
  inline JSObject** proxy() { return &proxy_; }
};

class IonDOMExitFrameLayout {
 protected:  // only to silence a clang warning about unused private fields
  ExitFooterFrame footer_;
  ExitFrameLayout exit_;
  JSObject* thisObj;

  // We need to split the Value into 2 fields of 32 bits, otherwise the C++
  // compiler may add some padding between the fields.
  uint32_t loCalleeResult_;
  uint32_t hiCalleeResult_;

 public:
  static ExitFrameType GetterType() { return ExitFrameType::IonDOMGetter; }
  static ExitFrameType SetterType() { return ExitFrameType::IonDOMSetter; }

  static inline size_t Size() { return sizeof(IonDOMExitFrameLayout); }

  static size_t offsetOfResult() {
    return offsetof(IonDOMExitFrameLayout, loCalleeResult_);
  }
  inline JS::Value* vp() {
    return reinterpret_cast<JS::Value*>(&loCalleeResult_);
  }
  inline JSObject** thisObjAddress() { return &thisObj; }
  inline bool isMethodFrame();
};

struct IonDOMMethodExitFrameLayoutTraits;

class IonDOMMethodExitFrameLayout {
 protected:  // only to silence a clang warning about unused private fields
  ExitFooterFrame footer_;
  ExitFrameLayout exit_;
  // This must be the last thing pushed, so as to stay common with
  // IonDOMExitFrameLayout.
  JSObject* thisObj_;
  JS::Value* argv_;
  uintptr_t argc_;

  // We need to split the Value into 2 fields of 32 bits, otherwise the C++
  // compiler may add some padding between the fields.
  uint32_t loCalleeResult_;
  uint32_t hiCalleeResult_;

  friend struct IonDOMMethodExitFrameLayoutTraits;

 public:
  static ExitFrameType Type() { return ExitFrameType::IonDOMMethod; }

  static inline size_t Size() { return sizeof(IonDOMMethodExitFrameLayout); }

  static size_t offsetOfResult() {
    return offsetof(IonDOMMethodExitFrameLayout, loCalleeResult_);
  }

  inline JS::Value* vp() {
    // The code in visitCallDOMNative depends on this static assert holding
    static_assert(
        offsetof(IonDOMMethodExitFrameLayout, loCalleeResult_) ==
        (offsetof(IonDOMMethodExitFrameLayout, argc_) + sizeof(uintptr_t)));
    return reinterpret_cast<JS::Value*>(&loCalleeResult_);
  }
  inline JSObject** thisObjAddress() { return &thisObj_; }
  inline uintptr_t argc() { return argc_; }
};

inline bool IonDOMExitFrameLayout::isMethodFrame() {
  return footer_.type() == IonDOMMethodExitFrameLayout::Type();
}

template <>
inline bool ExitFrameLayout::is<IonDOMExitFrameLayout>() {
  ExitFrameType type = footer()->type();
  return type == IonDOMExitFrameLayout::GetterType() ||
         type == IonDOMExitFrameLayout::SetterType() ||
         type == IonDOMMethodExitFrameLayout::Type();
}

template <>
inline IonDOMExitFrameLayout* ExitFrameLayout::as<IonDOMExitFrameLayout>() {
  MOZ_ASSERT(is<IonDOMExitFrameLayout>());
  return reinterpret_cast<IonDOMExitFrameLayout*>(footer());
}

struct IonDOMMethodExitFrameLayoutTraits {
  static const size_t offsetOfArgcFromArgv =
      offsetof(IonDOMMethodExitFrameLayout, argc_) -
      offsetof(IonDOMMethodExitFrameLayout, argv_);
};

// Cannot inherit implementation since we need to extend the top of
// ExitFrameLayout.
class CalledFromJitExitFrameLayout {
 protected:  // silence clang warning about unused private fields
  ExitFooterFrame footer_;
  JitFrameLayout exit_;

 public:
  static inline size_t Size() { return sizeof(CalledFromJitExitFrameLayout); }
  inline JitFrameLayout* jsFrame() { return &exit_; }
  static size_t offsetOfExitFrame() {
    return offsetof(CalledFromJitExitFrameLayout, exit_);
  }
};

class LazyLinkExitFrameLayout : public CalledFromJitExitFrameLayout {
 public:
  static ExitFrameType Type() { return ExitFrameType::LazyLink; }
};

class InterpreterStubExitFrameLayout : public CalledFromJitExitFrameLayout {
 public:
  static ExitFrameType Type() { return ExitFrameType::InterpreterStub; }
};

class WasmGenericJitEntryFrameLayout : CalledFromJitExitFrameLayout {
 public:
  static ExitFrameType Type() { return ExitFrameType::WasmGenericJitEntry; }
};

template <>
inline bool ExitFrameLayout::is<CalledFromJitExitFrameLayout>() {
  return is<InterpreterStubExitFrameLayout>() ||
         is<LazyLinkExitFrameLayout>() || is<WasmGenericJitEntryFrameLayout>();
}

template <>
inline CalledFromJitExitFrameLayout*
ExitFrameLayout::as<CalledFromJitExitFrameLayout>() {
  MOZ_ASSERT(is<CalledFromJitExitFrameLayout>());
  uint8_t* sp = reinterpret_cast<uint8_t*>(this);
  sp -= CalledFromJitExitFrameLayout::offsetOfExitFrame();
  return reinterpret_cast<CalledFromJitExitFrameLayout*>(sp);
}

class DirectWasmJitCallFrameLayout {
 protected:  // silence clang warning about unused private fields
  ExitFooterFrame footer_;
  ExitFrameLayout exit_;

 public:
  static ExitFrameType Type() { return ExitFrameType::DirectWasmJitCall; }
};

class ICStub;

class BaselineStubFrameLayout : public CommonFrameLayout {
  // Info on the stack
  //
  // +-----------------------+
  // |BaselineStubFrameLayout|
  // +-----------------------+
  // | - Descriptor          | => Marks end of FrameType::BaselineJS
  // | - Return address      |
  // | - CallerFramePtr      |
  // +-----------------------+
  // | - StubPtr             | Technically this last field is not part
  // +-----------------------+ of the frame layout.

 public:
  static constexpr size_t ICStubOffset = sizeof(void*);
  static constexpr int ICStubOffsetFromFP = -int(ICStubOffset);
  static constexpr size_t LocallyTracedValueOffset = 2 * sizeof(void*);

  static inline size_t Size() { return sizeof(BaselineStubFrameLayout); }

  ICStub* maybeStubPtr() {
    uint8_t* fp = reinterpret_cast<uint8_t*>(this);
    return *reinterpret_cast<ICStub**>(fp - ICStubOffset);
  }
  void setStubPtr(ICStub* stub) {
    MOZ_ASSERT(stub);
    uint8_t* fp = reinterpret_cast<uint8_t*>(this);
    *reinterpret_cast<ICStub**>(fp - ICStubOffset) = stub;
  }

  inline Value* locallyTracedValuePtr(size_t index) {
    uint8_t* fp = reinterpret_cast<uint8_t*>(this);
    return reinterpret_cast<Value*>(fp - LocallyTracedValueOffset -
                                    index * sizeof(Value));
  }
};

// An invalidation bailout stack is at the stack pointer for the callee frame.
class InvalidationBailoutStack {
  RegisterDump::FPUArray fpregs_;
  RegisterDump::GPRArray regs_;
  IonScript* ionScript_;
  uint8_t* osiPointReturnAddress_;

 public:
  uint8_t* sp() const {
    return (uint8_t*)this + sizeof(InvalidationBailoutStack);
  }
  JitFrameLayout* fp() const;
  MachineState machine() { return MachineState::FromBailout(regs_, fpregs_); }

  IonScript* ionScript() const { return ionScript_; }
  uint8_t* osiPointReturnAddress() const { return osiPointReturnAddress_; }
  static size_t offsetOfFpRegs() {
    return offsetof(InvalidationBailoutStack, fpregs_);
  }
  static size_t offsetOfRegs() {
    return offsetof(InvalidationBailoutStack, regs_);
  }

  void checkInvariants() const;
};

// Baseline requires one slot for this/argument type checks.
static const uint32_t MinJITStackSize = 1;

} /* namespace jit */
} /* namespace js */

#endif /* jit_JitFrames_h */
