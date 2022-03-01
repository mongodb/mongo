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
#include "jit/Registers.h"
#include "js/Id.h"
#include "js/TypeDecls.h"
#include "js/Value.h"

namespace js {
namespace jit {

enum class FrameType;
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
// CommonFrameLayout). The descriptor describes the size and type of the older
// (caller) frame, whereas the returnAddress describes the address the newer
// (callee) frame will return to.
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
// A frame descriptor word is organized into four sections:
//
//    high bits: [ frame size |
//                 has-cached-saved-frame bit |
///                frame header size|
//    low bits:    frame type ]
//
// * Frame Size: Size of caller frame
// * Has-cache-saved-frame bit: Used to power the LiveSavedFrameCache
//   optimization. See the comment in Activation.h
// * Frame header size: The number of words in a frame header (see
//   FrameLayout::Size())
// * Frame Type: BaselineJS, Exit, etc. (jit::FrameType)
//

static const uintptr_t FRAMETYPE_BITS = 4;
static const uintptr_t FRAMETYPE_MASK = (1 << FRAMETYPE_BITS) - 1;
static const uintptr_t FRAME_HEADER_SIZE_SHIFT = FRAMETYPE_BITS;
static const uintptr_t FRAME_HEADER_SIZE_BITS = 3;
static const uintptr_t FRAME_HEADER_SIZE_MASK =
    (1 << FRAME_HEADER_SIZE_BITS) - 1;
static const uintptr_t HASCACHEDSAVEDFRAME_BIT =
    1 << (FRAMETYPE_BITS + FRAME_HEADER_SIZE_BITS);
static const uintptr_t FRAMESIZE_SHIFT =
    FRAMETYPE_BITS + FRAME_HEADER_SIZE_BITS + 1 /* cached saved frame bit */;
static const uintptr_t FRAMESIZE_BITS = 32 - FRAMESIZE_SHIFT;
static const uintptr_t FRAMESIZE_MASK = (1 << FRAMESIZE_BITS) - 1;

// Ion frames have a few important numbers associated with them:
//      Local depth:    The number of bytes required to spill local variables.
//      Argument depth: The number of bytes required to push arguments and make
//                      a function call.
//      Slack:          A frame may temporarily use extra stack to resolve
//                      cycles.
//
// The (local + argument) depth determines the "fixed frame size". The fixed
// frame size is the distance between the stack pointer and the frame header.
// Thus, fixed >= (local + argument).
//
// In order to compress guards, we create shared jump tables that recover the
// script from the stack and recover a snapshot pointer based on which jump was
// taken. Thus, we create a jump table for each fixed frame size.
//
// Jump tables are big. To control the amount of jump tables we generate, each
// platform chooses how to segregate stack size classes based on its
// architecture.
//
// On some architectures, these jump tables are not used at all, or frame
// size segregation is not needed. Thus, there is an option for a frame to not
// have any frame size class, and to be totally dynamic.
static const uint32_t NO_FRAME_SIZE_CLASS_ID = uint32_t(-1);

class FrameSizeClass {
  uint32_t class_;

  explicit FrameSizeClass(uint32_t class_) : class_(class_) {}

 public:
  FrameSizeClass() = delete;

  static FrameSizeClass None() {
    return FrameSizeClass(NO_FRAME_SIZE_CLASS_ID);
  }
  static FrameSizeClass FromClass(uint32_t class_) {
    return FrameSizeClass(class_);
  }

  // These functions are implemented in specific CodeGenerator-* files.
  static FrameSizeClass FromDepth(uint32_t frameDepth);
  static FrameSizeClass ClassLimit();
  uint32_t frameSize() const;

  uint32_t classId() const {
    MOZ_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);
    return class_;
  }

  bool operator==(const FrameSizeClass& other) const {
    return class_ == other.class_;
  }
  bool operator!=(const FrameSizeClass& other) const {
    return class_ != other.class_;
  }
};

struct BaselineBailoutInfo;

// Data needed to recover from an exception.
struct ResumeFromException {
  static const uint32_t RESUME_ENTRY_FRAME = 0;
  static const uint32_t RESUME_CATCH = 1;
  static const uint32_t RESUME_FINALLY = 2;
  static const uint32_t RESUME_FORCED_RETURN = 3;
  static const uint32_t RESUME_BAILOUT = 4;
  static const uint32_t RESUME_WASM = 5;
  static const uint32_t RESUME_WASM_CATCH = 6;

  uint8_t* framePointer;
  uint8_t* stackPointer;
  uint8_t* target;
  uint32_t kind;

  // Value to push when resuming into a |finally| block.
  // Also used by Wasm to send the exception object to the throw stub.
  JS::Value exception;

  BaselineBailoutInfo* bailoutInfo;
};

void HandleException(ResumeFromException* rfe);

void EnsureBareExitFrame(JitActivation* act, JitFrameLayout* frame);

void TraceJitActivations(JSContext* cx, JSTracer* trc);

void UpdateJitActivationsForMinorGC(JSRuntime* rt);

static inline uint32_t EncodeFrameHeaderSize(size_t headerSize) {
  MOZ_ASSERT((headerSize % sizeof(uintptr_t)) == 0);

  uint32_t headerSizeWords = headerSize / sizeof(uintptr_t);
  MOZ_ASSERT(headerSizeWords <= FRAME_HEADER_SIZE_MASK);
  return headerSizeWords;
}

static inline uint32_t MakeFrameDescriptor(uint32_t frameSize, FrameType type,
                                           uint32_t headerSize) {
  MOZ_ASSERT(frameSize < FRAMESIZE_MASK);
  headerSize = EncodeFrameHeaderSize(headerSize);
  return 0 | (frameSize << FRAMESIZE_SHIFT) |
         (headerSize << FRAME_HEADER_SIZE_SHIFT) | uint32_t(type);
}

// Returns the JSScript associated with the topmost JIT frame.
JSScript* GetTopJitJSScript(JSContext* cx);

#ifdef JS_CODEGEN_MIPS32
uint8_t* alignDoubleSpill(uint8_t* pointer);
#else
inline uint8_t* alignDoubleSpill(uint8_t* pointer) {
  // This is NO-OP on non-MIPS platforms.
  return pointer;
}
#endif

// Layout of the frame prefix. This assumes the stack architecture grows down.
// If this is ever not the case, we'll have to refactor.
class CommonFrameLayout {
  uint8_t* returnAddress_;
  uintptr_t descriptor_;

 public:
  static size_t offsetOfDescriptor() {
    return offsetof(CommonFrameLayout, descriptor_);
  }
  uintptr_t descriptor() const { return descriptor_; }
  static size_t offsetOfReturnAddress() {
    return offsetof(CommonFrameLayout, returnAddress_);
  }
  FrameType prevType() const { return FrameType(descriptor_ & FRAMETYPE_MASK); }
  void changePrevType(FrameType type) {
    descriptor_ &= ~FRAMETYPE_MASK;
    descriptor_ |= uintptr_t(type);
  }
  size_t prevFrameLocalSize() const { return descriptor_ >> FRAMESIZE_SHIFT; }
  size_t headerSize() const {
    return sizeof(uintptr_t) *
           ((descriptor_ >> FRAME_HEADER_SIZE_SHIFT) & FRAME_HEADER_SIZE_MASK);
  }
  bool hasCachedSavedFrame() const {
    return descriptor_ & HASCACHEDSAVEDFRAME_BIT;
  }
  void setHasCachedSavedFrame() { descriptor_ |= HASCACHEDSAVEDFRAME_BIT; }
  void clearHasCachedSavedFrame() { descriptor_ &= ~HASCACHEDSAVEDFRAME_BIT; }
  uint8_t* returnAddress() const { return returnAddress_; }
  void setReturnAddress(uint8_t* addr) { returnAddress_ = addr; }
};

class JitFrameLayout : public CommonFrameLayout {
  CalleeToken calleeToken_;
  uintptr_t numActualArgs_;

 public:
  CalleeToken calleeToken() const { return calleeToken_; }
  void replaceCalleeToken(CalleeToken calleeToken) {
    calleeToken_ = calleeToken;
  }

  static size_t offsetOfCalleeToken() {
    return offsetof(JitFrameLayout, calleeToken_);
  }
  static size_t offsetOfNumActualArgs() {
    return offsetof(JitFrameLayout, numActualArgs_);
  }
  static size_t offsetOfThis() { return sizeof(JitFrameLayout); }
  static size_t offsetOfEvalNewTarget() { return sizeof(JitFrameLayout); }
  static size_t offsetOfActualArgs() {
    return offsetOfThis() + sizeof(JS::Value);
  }
  static size_t offsetOfActualArg(size_t arg) {
    return offsetOfActualArgs() + arg * sizeof(JS::Value);
  }

  JS::Value thisv() {
    MOZ_ASSERT(CalleeTokenIsFunction(calleeToken()));
    return argv()[0];
  }
  JS::Value* argv() {
    MOZ_ASSERT(CalleeTokenIsFunction(calleeToken()));
    return (JS::Value*)(this + 1);
  }
  uintptr_t numActualArgs() const { return numActualArgs_; }

  // Computes a reference to a stack or argument slot, where a slot is a
  // distance from the base frame pointer, as would be used for LStackSlot
  // or LArgument.
  uintptr_t* slotRef(SafepointSlotEntry where);

  static inline size_t Size() { return sizeof(JitFrameLayout); }
};

class RectifierFrameLayout : public JitFrameLayout {
 public:
  static inline size_t Size() { return sizeof(RectifierFrameLayout); }
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
  JitCode** stubCode() { return &stubCode_; }
  static size_t Size() { return sizeof(IonICCallFrameLayout); }
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
  InterpreterStub = 0xFC,
  VMFunction = 0xFD,
  LazyLink = 0xFE,
  Bare = 0xFF,
};

// GC related data used to keep alive data surrounding the Exit frame.
class ExitFooterFrame {
  // Stores the ExitFrameType or, for ExitFrameType::VMFunction, the
  // VMFunctionData*.
  uintptr_t data_;

 public:
  static inline size_t Size() { return sizeof(ExitFooterFrame); }
  void setBareExitFrame() { data_ = uintptr_t(ExitFrameType::Bare); }
  ExitFrameType type() const {
    static_assert(sizeof(ExitFrameType) == sizeof(uint8_t),
                  "Code assumes ExitFrameType fits in a byte");
    if (data_ > UINT8_MAX) {
      return ExitFrameType::VMFunction;
    }
    MOZ_ASSERT(ExitFrameType(data_) != ExitFrameType::VMFunction);
    return ExitFrameType(data_);
  }
  inline const VMFunctionData* function() const {
    MOZ_ASSERT(type() == ExitFrameType::VMFunction);
    return reinterpret_cast<const VMFunctionData*>(data_);
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

class JitStubFrameLayout : public CommonFrameLayout {
  /* clang-format off */
    // Info on the stack
    //
    // --------------------
    // |JitStubFrameLayout|
    // +------------------+
    // | - Descriptor     | => Marks end of FrameType::IonJS
    // | - returnaddres   |
    // +------------------+
    // | - StubPtr        | => First thing pushed in a stub only when the stub will do
    // --------------------    a vmcall. Else we cannot have JitStubFrame. But technically
    //                         not a member of the layout.
  /* clang-format on */

 public:
  static size_t Size() { return sizeof(JitStubFrameLayout); }

  static inline int reverseOffsetOfStubPtr() { return -int(sizeof(void*)); }

  inline ICStub* maybeStubPtr() {
    uint8_t* fp = reinterpret_cast<uint8_t*>(this);
    return *reinterpret_cast<ICStub**>(fp + reverseOffsetOfStubPtr());
  }
};

class BaselineStubFrameLayout : public JitStubFrameLayout {
  /* clang-format off */
    // Info on the stack
    //
    // -------------------------
    // |BaselineStubFrameLayout|
    // +-----------------------+
    // | - Descriptor          | => Marks end of FrameType::BaselineJS
    // | - returnaddres        |
    // +-----------------------+
    // | - StubPtr             | => First thing pushed in a stub only when the stub will do
    // +-----------------------+    a vmcall. Else we cannot have BaselineStubFrame.
    // | - FramePtr            | => Baseline stubs also need to push the frame ptr when doing
    // -------------------------    a vmcall.
    //                              Technically these last two variables are not part of the
    //                              layout.
  /* clang-format on */

 public:
  static inline size_t Size() { return sizeof(BaselineStubFrameLayout); }

  static inline int reverseOffsetOfSavedFramePtr() {
    return -int(2 * sizeof(void*));
  }

  void* reverseSavedFramePtr() {
    uint8_t* addr = ((uint8_t*)this) + reverseOffsetOfSavedFramePtr();
    return *(void**)addr;
  }

  inline void setStubPtr(ICStub* stub) {
    uint8_t* fp = reinterpret_cast<uint8_t*>(this);
    *reinterpret_cast<ICStub**>(fp + reverseOffsetOfStubPtr()) = stub;
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

void GetPcScript(JSContext* cx, JSScript** scriptRes, jsbytecode** pcRes);

// Baseline requires one slot for this/argument type checks.
static const uint32_t MinJITStackSize = 1;

} /* namespace jit */
} /* namespace js */

#endif /* jit_JitFrames_h */
