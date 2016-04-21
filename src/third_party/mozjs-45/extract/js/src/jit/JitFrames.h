/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitFrames_h
#define jit_JitFrames_h

#include <stdint.h>

#include "jscntxt.h"
#include "jsfun.h"

#include "jit/JitFrameIterator.h"
#include "jit/Safepoints.h"

namespace js {
namespace jit {

enum CalleeTokenTag
{
    CalleeToken_Function = 0x0, // untagged
    CalleeToken_FunctionConstructing = 0x1,
    CalleeToken_Script = 0x2
};

static const uintptr_t CalleeTokenMask = ~uintptr_t(0x3);

static inline CalleeTokenTag
GetCalleeTokenTag(CalleeToken token)
{
    CalleeTokenTag tag = CalleeTokenTag(uintptr_t(token) & 0x3);
    MOZ_ASSERT(tag <= CalleeToken_Script);
    return tag;
}
static inline CalleeToken
CalleeToToken(JSFunction* fun, bool constructing)
{
    CalleeTokenTag tag = constructing ? CalleeToken_FunctionConstructing : CalleeToken_Function;
    return CalleeToken(uintptr_t(fun) | uintptr_t(tag));
}
static inline CalleeToken
CalleeToToken(JSScript* script)
{
    return CalleeToken(uintptr_t(script) | uintptr_t(CalleeToken_Script));
}
static inline bool
CalleeTokenIsFunction(CalleeToken token)
{
    CalleeTokenTag tag = GetCalleeTokenTag(token);
    return tag == CalleeToken_Function || tag == CalleeToken_FunctionConstructing;
}
static inline bool
CalleeTokenIsConstructing(CalleeToken token)
{
    return GetCalleeTokenTag(token) == CalleeToken_FunctionConstructing;
}
static inline JSFunction*
CalleeTokenToFunction(CalleeToken token)
{
    MOZ_ASSERT(CalleeTokenIsFunction(token));
    return (JSFunction*)(uintptr_t(token) & CalleeTokenMask);
}
static inline JSScript*
CalleeTokenToScript(CalleeToken token)
{
    MOZ_ASSERT(GetCalleeTokenTag(token) == CalleeToken_Script);
    return (JSScript*)(uintptr_t(token) & CalleeTokenMask);
}
static inline bool
CalleeTokenIsModuleScript(CalleeToken token)
{
    CalleeTokenTag tag = GetCalleeTokenTag(token);
    return tag == CalleeToken_Script && CalleeTokenToScript(token)->module();
}

static inline JSScript*
ScriptFromCalleeToken(CalleeToken token)
{
    switch (GetCalleeTokenTag(token)) {
      case CalleeToken_Script:
        return CalleeTokenToScript(token);
      case CalleeToken_Function:
      case CalleeToken_FunctionConstructing:
        return CalleeTokenToFunction(token)->nonLazyScript();
    }
    MOZ_CRASH("invalid callee token tag");
}

// In between every two frames lies a small header describing both frames. This
// header, minimally, contains a returnAddress word and a descriptor word. The
// descriptor describes the size and type of the previous frame, whereas the
// returnAddress describes the address the newer frame (the callee) will return
// to. The exact mechanism in which frames are laid out is architecture
// dependent.
//
// Two special frame types exist. Entry frames begin an ion activation, and
// therefore there is exactly one per activation of jit::Cannon. Exit frames
// are necessary to leave JIT code and enter C++, and thus, C++ code will
// always begin iterating from the topmost exit frame.

class LSafepoint;

// Two-tuple that lets you look up the safepoint entry given the
// displacement of a call instruction within the JIT code.
class SafepointIndex
{
    // The displacement is the distance from the first byte of the JIT'd code
    // to the return address (of the call that the safepoint was generated for).
    uint32_t displacement_;

    union {
        LSafepoint* safepoint_;

        // Offset to the start of the encoded safepoint in the safepoint stream.
        uint32_t safepointOffset_;
    };

#ifdef DEBUG
    bool resolved;
#endif

  public:
    SafepointIndex(uint32_t displacement, LSafepoint* safepoint)
      : displacement_(displacement),
        safepoint_(safepoint)
#ifdef DEBUG
      , resolved(false)
#endif
    { }

    void resolve();

    LSafepoint* safepoint() {
        MOZ_ASSERT(!resolved);
        return safepoint_;
    }
    uint32_t displacement() const {
        return displacement_;
    }
    uint32_t safepointOffset() const {
        return safepointOffset_;
    }
    void adjustDisplacement(uint32_t offset) {
        MOZ_ASSERT(offset >= displacement_);
        displacement_ = offset;
    }
    inline SnapshotOffset snapshotOffset() const;
    inline bool hasSnapshotOffset() const;
};

class MacroAssembler;
// The OSI point is patched to a call instruction. Therefore, the
// returnPoint for an OSI call is the address immediately following that
// call instruction. The displacement of that point within the assembly
// buffer is the |returnPointDisplacement|.
class OsiIndex
{
    uint32_t callPointDisplacement_;
    uint32_t snapshotOffset_;

  public:
    OsiIndex(uint32_t callPointDisplacement, uint32_t snapshotOffset)
      : callPointDisplacement_(callPointDisplacement),
        snapshotOffset_(snapshotOffset)
    { }

    uint32_t returnPointDisplacement() const;
    uint32_t callPointDisplacement() const {
        return callPointDisplacement_;
    }
    uint32_t snapshotOffset() const {
        return snapshotOffset_;
    }
};

// The layout of an Ion frame on the C stack is roughly:
//      argN     _
//      ...       \ - These are jsvals
//      arg0      /
//   -3 this    _/
//   -2 callee
//   -1 descriptor
//    0 returnAddress
//   .. locals ..

// The descriptor is organized into three sections:
// [ frame size | has cached saved frame bit | frame type ]
// < highest - - - - - - - - - - - - - - lowest >
static const uintptr_t FRAMESIZE_SHIFT = 5;
static const uintptr_t HASCACHEDSAVEDFRAME_BIT = 1 << 4;
static const uintptr_t FRAMETYPE_BITS = 4;

// Ion frames have a few important numbers associated with them:
//      Local depth:    The number of bytes required to spill local variables.
//      Argument depth: The number of bytes required to push arguments and make
//                      a function call.
//      Slack:          A frame may temporarily use extra stack to resolve cycles.
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

class FrameSizeClass
{
    uint32_t class_;

    explicit FrameSizeClass(uint32_t class_) : class_(class_)
    { }

  public:
    FrameSizeClass()
    { }

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

    bool operator ==(const FrameSizeClass& other) const {
        return class_ == other.class_;
    }
    bool operator !=(const FrameSizeClass& other) const {
        return class_ != other.class_;
    }
};

struct BaselineBailoutInfo;

// Data needed to recover from an exception.
struct ResumeFromException
{
    static const uint32_t RESUME_ENTRY_FRAME = 0;
    static const uint32_t RESUME_CATCH = 1;
    static const uint32_t RESUME_FINALLY = 2;
    static const uint32_t RESUME_FORCED_RETURN = 3;
    static const uint32_t RESUME_BAILOUT = 4;

    uint8_t* framePointer;
    uint8_t* stackPointer;
    uint8_t* target;
    uint32_t kind;

    // Value to push when resuming into a |finally| block.
    Value exception;

    BaselineBailoutInfo* bailoutInfo;
};

void HandleException(ResumeFromException* rfe);

void EnsureExitFrame(CommonFrameLayout* frame);

void MarkJitActivations(JSRuntime* rt, JSTracer* trc);
void MarkIonCompilerRoots(JSTracer* trc);

JSCompartment*
TopmostIonActivationCompartment(JSRuntime* rt);

void UpdateJitActivationsForMinorGC(JSRuntime* rt, JSTracer* trc);

static inline uint32_t
MakeFrameDescriptor(uint32_t frameSize, FrameType type)
{
    return 0 | (frameSize << FRAMESIZE_SHIFT) | type;
}

// Returns the JSScript associated with the topmost JIT frame.
inline JSScript*
GetTopJitJSScript(JSContext* cx)
{
    JitFrameIterator iter(cx);
    MOZ_ASSERT(iter.type() == JitFrame_Exit);
    ++iter;

    if (iter.isBaselineStub()) {
        ++iter;
        MOZ_ASSERT(iter.isBaselineJS());
    }

    MOZ_ASSERT(iter.isScripted());
    return iter.script();
}

#ifdef JS_CODEGEN_MIPS32
uint8_t* alignDoubleSpillWithOffset(uint8_t* pointer, int32_t offset);
#else
inline uint8_t*
alignDoubleSpillWithOffset(uint8_t* pointer, int32_t offset)
{
    // This is NO-OP on non-MIPS platforms.
    return pointer;
}
#endif

// Layout of the frame prefix. This assumes the stack architecture grows down.
// If this is ever not the case, we'll have to refactor.
class CommonFrameLayout
{
    uint8_t* returnAddress_;
    uintptr_t descriptor_;

    static const uintptr_t FrameTypeMask = (1 << FRAMETYPE_BITS) - 1;

  public:
    static size_t offsetOfDescriptor() {
        return offsetof(CommonFrameLayout, descriptor_);
    }
    uintptr_t descriptor() const {
        return descriptor_;
    }
    static size_t offsetOfReturnAddress() {
        return offsetof(CommonFrameLayout, returnAddress_);
    }
    FrameType prevType() const {
        return FrameType(descriptor_ & FrameTypeMask);
    }
    void changePrevType(FrameType type) {
        descriptor_ &= ~FrameTypeMask;
        descriptor_ |= type;
    }
    size_t prevFrameLocalSize() const {
        return descriptor_ >> FRAMESIZE_SHIFT;
    }
    void setFrameDescriptor(size_t size, FrameType type) {
        descriptor_ = 0 | (size << FRAMESIZE_SHIFT) | type;
    }
    bool hasCachedSavedFrame() const {
        return descriptor_ & HASCACHEDSAVEDFRAME_BIT;
    }
    void setHasCachedSavedFrame() {
        descriptor_ |= HASCACHEDSAVEDFRAME_BIT;
    }
    uint8_t* returnAddress() const {
        return returnAddress_;
    }
    void setReturnAddress(uint8_t* addr) {
        returnAddress_ = addr;
    }
};

class JitFrameLayout : public CommonFrameLayout
{
    CalleeToken calleeToken_;
    uintptr_t numActualArgs_;

  public:
    CalleeToken calleeToken() const {
        return calleeToken_;
    }
    void replaceCalleeToken(CalleeToken calleeToken) {
        calleeToken_ = calleeToken;
    }

    static size_t offsetOfCalleeToken() {
        return offsetof(JitFrameLayout, calleeToken_);
    }
    static size_t offsetOfNumActualArgs() {
        return offsetof(JitFrameLayout, numActualArgs_);
    }
    static size_t offsetOfThis() {
        return sizeof(JitFrameLayout);
    }
    static size_t offsetOfEvalNewTarget() {
        return sizeof(JitFrameLayout);
    }
    static size_t offsetOfActualArgs() {
        return offsetOfThis() + sizeof(Value);
    }
    static size_t offsetOfActualArg(size_t arg) {
        return offsetOfActualArgs() + arg * sizeof(Value);
    }

    Value thisv() {
        MOZ_ASSERT(CalleeTokenIsFunction(calleeToken()));
        return argv()[0];
    }
    Value* argv() {
        MOZ_ASSERT(CalleeTokenIsFunction(calleeToken()));
        return (Value*)(this + 1);
    }
    uintptr_t numActualArgs() const {
        return numActualArgs_;
    }

    // Computes a reference to a stack or argument slot, where a slot is a
    // distance from the base frame pointer, as would be used for LStackSlot
    // or LArgument.
    uintptr_t* slotRef(SafepointSlotEntry where);

    static inline size_t Size() {
        return sizeof(JitFrameLayout);
    }
};

// this is the layout of the frame that is used when we enter Ion code from platform ABI code
class EntryFrameLayout : public JitFrameLayout
{
  public:
    static inline size_t Size() {
        return sizeof(EntryFrameLayout);
    }
};

class RectifierFrameLayout : public JitFrameLayout
{
  public:
    static inline size_t Size() {
        return sizeof(RectifierFrameLayout);
    }
};

class IonAccessorICFrameLayout : public CommonFrameLayout
{
  protected:
    // Pointer to root the stub's JitCode.
    JitCode* stubCode_;

  public:
    JitCode** stubCode() {
        return &stubCode_;
    }
    static size_t Size() {
        return sizeof(IonAccessorICFrameLayout);
    }
};

// The callee token is now dead.
class IonUnwoundRectifierFrameLayout : public RectifierFrameLayout
{
  public:
    static inline size_t Size() {
        // It is not necessary to accout for an extra callee token here because
        // sizeof(ExitFrameLayout) == sizeof(RectifierFrameLayout) due to
        // extra padding.
        return sizeof(IonUnwoundRectifierFrameLayout);
    }
};

// GC related data used to keep alive data surrounding the Exit frame.
class ExitFooterFrame
{
    const VMFunction* function_;
    JitCode* jitCode_;

  public:
    static inline size_t Size() {
        return sizeof(ExitFooterFrame);
    }
    inline JitCode* jitCode() const {
        return jitCode_;
    }
    inline JitCode** addressOfJitCode() {
        return &jitCode_;
    }
    inline const VMFunction* function() const {
        return function_;
    }

    // This should only be called for function()->outParam == Type_Handle
    template <typename T>
    T* outParam() {
        uint8_t* address = reinterpret_cast<uint8_t*>(this);
        address = alignDoubleSpillWithOffset(address, sizeof(intptr_t));
        return reinterpret_cast<T*>(address - sizeof(T));
    }
};

class NativeExitFrameLayout;
class IonOOLNativeExitFrameLayout;
class IonOOLPropertyOpExitFrameLayout;
class IonOOLProxyExitFrameLayout;
class IonDOMExitFrameLayout;

enum ExitFrameTokenValues
{
    CallNativeExitFrameLayoutToken        = 0x0,
    ConstructNativeExitFrameLayoutToken   = 0x1,
    IonDOMExitFrameLayoutGetterToken      = 0x2,
    IonDOMExitFrameLayoutSetterToken      = 0x3,
    IonDOMMethodExitFrameLayoutToken      = 0x4,
    IonOOLNativeExitFrameLayoutToken      = 0x5,
    IonOOLPropertyOpExitFrameLayoutToken  = 0x6,
    IonOOLSetterOpExitFrameLayoutToken    = 0x7,
    IonOOLProxyExitFrameLayoutToken       = 0x8,
    LazyLinkExitFrameLayoutToken          = 0xFE,
    ExitFrameLayoutBareToken              = 0xFF
};

// this is the frame layout when we are exiting ion code, and about to enter platform ABI code
class ExitFrameLayout : public CommonFrameLayout
{
    inline uint8_t* top() {
        return reinterpret_cast<uint8_t*>(this + 1);
    }

  public:
    // Pushed for "bare" fake exit frames that have no GC things on stack to be
    // marked.
    static JitCode* BareToken() { return (JitCode*)ExitFrameLayoutBareToken; }

    static inline size_t Size() {
        return sizeof(ExitFrameLayout);
    }
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
        MOZ_ASSERT(footer()->jitCode() != nullptr);
        return top();
    }

    inline bool isWrapperExit() {
        return footer()->function() != nullptr;
    }
    inline bool isBareExit() {
        return footer()->jitCode() == BareToken();
    }

    // See the various exit frame layouts below.
    template <typename T> inline bool is() {
        return footer()->jitCode() == T::Token();
    }
    template <typename T> inline T* as() {
        MOZ_ASSERT(this->is<T>());
        return reinterpret_cast<T*>(footer());
    }
};

// Cannot inherit implementation since we need to extend the top of
// ExitFrameLayout.
class NativeExitFrameLayout
{
  protected: // only to silence a clang warning about unused private fields
    ExitFooterFrame footer_;
    ExitFrameLayout exit_;
    uintptr_t argc_;

    // We need to split the Value into 2 fields of 32 bits, otherwise the C++
    // compiler may add some padding between the fields.
    uint32_t loCalleeResult_;
    uint32_t hiCalleeResult_;

  public:
    static inline size_t Size() {
        return sizeof(NativeExitFrameLayout);
    }

    static size_t offsetOfResult() {
        return offsetof(NativeExitFrameLayout, loCalleeResult_);
    }
    inline Value* vp() {
        return reinterpret_cast<Value*>(&loCalleeResult_);
    }
    inline uintptr_t argc() const {
        return argc_;
    }
};

class CallNativeExitFrameLayout : public NativeExitFrameLayout
{
  public:
    static JitCode* Token() { return (JitCode*)CallNativeExitFrameLayoutToken; }
};

class ConstructNativeExitFrameLayout : public NativeExitFrameLayout
{
  public:
    static JitCode* Token() { return (JitCode*)ConstructNativeExitFrameLayoutToken; }
};

template<>
inline bool
ExitFrameLayout::is<NativeExitFrameLayout>()
{
    return is<CallNativeExitFrameLayout>() || is<ConstructNativeExitFrameLayout>();
}

class IonOOLNativeExitFrameLayout
{
  protected: // only to silence a clang warning about unused private fields
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
    static JitCode* Token() { return (JitCode*)IonOOLNativeExitFrameLayoutToken; }

    static inline size_t Size(size_t argc) {
        // The frame accounts for the callee/result and |this|, so we only need args.
        return sizeof(IonOOLNativeExitFrameLayout) + (argc * sizeof(Value));
    }

    static size_t offsetOfResult() {
        return offsetof(IonOOLNativeExitFrameLayout, loCalleeResult_);
    }

    inline JitCode** stubCode() {
        return &stubCode_;
    }
    inline Value* vp() {
        return reinterpret_cast<Value*>(&loCalleeResult_);
    }
    inline Value* thisp() {
        return reinterpret_cast<Value*>(&loThis_);
    }
    inline uintptr_t argc() const {
        return argc_;
    }
};

class IonOOLPropertyOpExitFrameLayout
{
  protected:
    ExitFooterFrame footer_;
    ExitFrameLayout exit_;

    // Object for HandleObject
    JSObject* obj_;

    // id for HandleId
    jsid id_;

    // space for MutableHandleValue result
    // use two uint32_t so compiler doesn't align.
    uint32_t vp0_;
    uint32_t vp1_;

    // pointer to root the stub's JitCode
    JitCode* stubCode_;

  public:
    static JitCode* Token() { return (JitCode*)IonOOLPropertyOpExitFrameLayoutToken; }

    static inline size_t Size() {
        return sizeof(IonOOLPropertyOpExitFrameLayout);
    }

    static size_t offsetOfObject() {
        return offsetof(IonOOLPropertyOpExitFrameLayout, obj_);
    }

    static size_t offsetOfId() {
        return offsetof(IonOOLPropertyOpExitFrameLayout, id_);
    }

    static size_t offsetOfResult() {
        return offsetof(IonOOLPropertyOpExitFrameLayout, vp0_);
    }

    inline JitCode** stubCode() {
        return &stubCode_;
    }
    inline Value* vp() {
        return reinterpret_cast<Value*>(&vp0_);
    }
    inline jsid* id() {
        return &id_;
    }
    inline JSObject** obj() {
        return &obj_;
    }
};

class IonOOLSetterOpExitFrameLayout : public IonOOLPropertyOpExitFrameLayout
{
  protected: // only to silence a clang warning about unused private fields
    JS::ObjectOpResult result_;

  public:
    static JitCode* Token() { return (JitCode*)IonOOLSetterOpExitFrameLayoutToken; }

    static size_t offsetOfObjectOpResult() {
        return offsetof(IonOOLSetterOpExitFrameLayout, result_);
    }

    static size_t Size() {
        return sizeof(IonOOLSetterOpExitFrameLayout);
    }
};

// ProxyGetProperty(JSContext* cx, HandleObject proxy, HandleId id, MutableHandleValue vp)
// ProxyCallProperty(JSContext* cx, HandleObject proxy, HandleId id, MutableHandleValue vp)
// ProxySetProperty(JSContext* cx, HandleObject proxy, HandleId id, MutableHandleValue vp,
//                  bool strict)
class IonOOLProxyExitFrameLayout
{
  protected: // only to silence a clang warning about unused private fields
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
    static JitCode* Token() { return (JitCode*)IonOOLProxyExitFrameLayoutToken; }

    static inline size_t Size() {
        return sizeof(IonOOLProxyExitFrameLayout);
    }

    static size_t offsetOfResult() {
        return offsetof(IonOOLProxyExitFrameLayout, vp0_);
    }

    inline JitCode** stubCode() {
        return &stubCode_;
    }
    inline Value* vp() {
        return reinterpret_cast<Value*>(&vp0_);
    }
    inline jsid* id() {
        return &id_;
    }
    inline JSObject** proxy() {
        return &proxy_;
    }
};

class IonDOMExitFrameLayout
{
  protected: // only to silence a clang warning about unused private fields
    ExitFooterFrame footer_;
    ExitFrameLayout exit_;
    JSObject* thisObj;

    // We need to split the Value into 2 fields of 32 bits, otherwise the C++
    // compiler may add some padding between the fields.
    uint32_t loCalleeResult_;
    uint32_t hiCalleeResult_;

  public:
    static JitCode* GetterToken() { return (JitCode*)IonDOMExitFrameLayoutGetterToken; }
    static JitCode* SetterToken() { return (JitCode*)IonDOMExitFrameLayoutSetterToken; }

    static inline size_t Size() {
        return sizeof(IonDOMExitFrameLayout);
    }

    static size_t offsetOfResult() {
        return offsetof(IonDOMExitFrameLayout, loCalleeResult_);
    }
    inline Value* vp() {
        return reinterpret_cast<Value*>(&loCalleeResult_);
    }
    inline JSObject** thisObjAddress() {
        return &thisObj;
    }
    inline bool isMethodFrame();
};

struct IonDOMMethodExitFrameLayoutTraits;

class IonDOMMethodExitFrameLayout
{
  protected: // only to silence a clang warning about unused private fields
    ExitFooterFrame footer_;
    ExitFrameLayout exit_;
    // This must be the last thing pushed, so as to stay common with
    // IonDOMExitFrameLayout.
    JSObject* thisObj_;
    Value* argv_;
    uintptr_t argc_;

    // We need to split the Value into 2 fields of 32 bits, otherwise the C++
    // compiler may add some padding between the fields.
    uint32_t loCalleeResult_;
    uint32_t hiCalleeResult_;

    friend struct IonDOMMethodExitFrameLayoutTraits;

  public:
    static JitCode* Token() { return (JitCode*)IonDOMMethodExitFrameLayoutToken; }

    static inline size_t Size() {
        return sizeof(IonDOMMethodExitFrameLayout);
    }

    static size_t offsetOfResult() {
        return offsetof(IonDOMMethodExitFrameLayout, loCalleeResult_);
    }

    inline Value* vp() {
        // The code in visitCallDOMNative depends on this static assert holding
        JS_STATIC_ASSERT(offsetof(IonDOMMethodExitFrameLayout, loCalleeResult_) ==
                         (offsetof(IonDOMMethodExitFrameLayout, argc_) + sizeof(uintptr_t)));
        return reinterpret_cast<Value*>(&loCalleeResult_);
    }
    inline JSObject** thisObjAddress() {
        return &thisObj_;
    }
    inline uintptr_t argc() {
        return argc_;
    }
};

inline bool
IonDOMExitFrameLayout::isMethodFrame()
{
    return footer_.jitCode() == IonDOMMethodExitFrameLayout::Token();
}

template <>
inline bool
ExitFrameLayout::is<IonDOMExitFrameLayout>()
{
    JitCode* code = footer()->jitCode();
    return
        code == IonDOMExitFrameLayout::GetterToken() ||
        code == IonDOMExitFrameLayout::SetterToken() ||
        code == IonDOMMethodExitFrameLayout::Token();
}

template <>
inline IonDOMExitFrameLayout*
ExitFrameLayout::as<IonDOMExitFrameLayout>()
{
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
class LazyLinkExitFrameLayout
{
  protected: // silence clang warning about unused private fields
    JitCode* stubCode_;
    ExitFooterFrame footer_;
    JitFrameLayout exit_;

  public:
    static JitCode* Token() { return (JitCode*) LazyLinkExitFrameLayoutToken; }

    static inline size_t Size() {
        return sizeof(LazyLinkExitFrameLayout);
    }

    inline JitCode** stubCode() {
        return &stubCode_;
    }
    inline JitFrameLayout* jsFrame() {
        return &exit_;
    }
    static size_t offsetOfExitFrame() {
        return offsetof(LazyLinkExitFrameLayout, exit_);
    }
};

template <>
inline LazyLinkExitFrameLayout*
ExitFrameLayout::as<LazyLinkExitFrameLayout>()
{
    MOZ_ASSERT(is<LazyLinkExitFrameLayout>());
    uint8_t* sp = reinterpret_cast<uint8_t*>(this);
    sp -= LazyLinkExitFrameLayout::offsetOfExitFrame();
    return reinterpret_cast<LazyLinkExitFrameLayout*>(sp);
}

class ICStub;

class JitStubFrameLayout : public CommonFrameLayout
{
    // Info on the stack
    //
    // --------------------
    // |JitStubFrameLayout|
    // +------------------+
    // | - Descriptor     | => Marks end of JitFrame_IonJS
    // | - returnaddres   |
    // +------------------+
    // | - StubPtr        | => First thing pushed in a stub only when the stub will do
    // --------------------    a vmcall. Else we cannot have JitStubFrame. But technically
    //                         not a member of the layout.

  public:
    static size_t Size() {
        return sizeof(JitStubFrameLayout);
    }

    static inline int reverseOffsetOfStubPtr() {
        return -int(sizeof(void*));
    }

    inline ICStub* maybeStubPtr() {
        uint8_t* fp = reinterpret_cast<uint8_t*>(this);
        return *reinterpret_cast<ICStub**>(fp + reverseOffsetOfStubPtr());
    }
};

class BaselineStubFrameLayout : public JitStubFrameLayout
{
    // Info on the stack
    //
    // -------------------------
    // |BaselineStubFrameLayout|
    // +-----------------------+
    // | - Descriptor          | => Marks end of JitFrame_BaselineJS
    // | - returnaddres        |
    // +-----------------------+
    // | - StubPtr             | => First thing pushed in a stub only when the stub will do
    // +-----------------------+    a vmcall. Else we cannot have BaselineStubFrame.
    // | - FramePtr            | => Baseline stubs also need to push the frame ptr when doing
    // -------------------------    a vmcall.
    //                              Technically these last two variables are not part of the
    //                              layout.

  public:
    static inline size_t Size() {
        return sizeof(BaselineStubFrameLayout);
    }

    static inline int reverseOffsetOfSavedFramePtr() {
        return -int(2 * sizeof(void*));
    }

    void* reverseSavedFramePtr() {
        uint8_t* addr = ((uint8_t*) this) + reverseOffsetOfSavedFramePtr();
        return *(void**)addr;
    }

    inline void setStubPtr(ICStub* stub) {
        uint8_t* fp = reinterpret_cast<uint8_t*>(this);
        *reinterpret_cast<ICStub**>(fp + reverseOffsetOfStubPtr()) = stub;
    }
};

// An invalidation bailout stack is at the stack pointer for the callee frame.
class InvalidationBailoutStack
{
    RegisterDump::FPUArray fpregs_;
    RegisterDump::GPRArray regs_;
    IonScript*  ionScript_;
    uint8_t*      osiPointReturnAddress_;

  public:
    uint8_t* sp() const {
        return (uint8_t*) this + sizeof(InvalidationBailoutStack);
    }
    JitFrameLayout* fp() const;
    MachineState machine() {
        return MachineState::FromBailout(regs_, fpregs_);
    }

    IonScript* ionScript() const {
        return ionScript_;
    }
    uint8_t* osiPointReturnAddress() const {
        return osiPointReturnAddress_;
    }
    static size_t offsetOfFpRegs() {
        return offsetof(InvalidationBailoutStack, fpregs_);
    }
    static size_t offsetOfRegs() {
        return offsetof(InvalidationBailoutStack, regs_);
    }

    void checkInvariants() const;
};

void
GetPcScript(JSContext* cx, JSScript** scriptRes, jsbytecode** pcRes);

CalleeToken
MarkCalleeToken(JSTracer* trc, CalleeToken token);

// The minimum stack size is two. Two slots are needed because INITGLEXICAL
// (stack depth 1) is compiled as a SETPROP (stack depth 2) on the global
// lexical scope. Baseline also requires one slot for this/argument type
// checks.
static const uint32_t MinJITStackSize = 2;

} /* namespace jit */
} /* namespace js */

#endif /* jit_JitFrames_h */
