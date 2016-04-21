/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_BaselineFrame_h
#define jit_BaselineFrame_h

#include "jit/JitFrames.h"
#include "vm/Stack.h"

namespace js {
namespace jit {

struct BaselineDebugModeOSRInfo;

// The stack looks like this, fp is the frame pointer:
//
// fp+y   arguments
// fp+x   JitFrameLayout (frame header)
// fp  => saved frame pointer
// fp-x   BaselineFrame
//        locals
//        stack values

// Eval frames
//
// Like js::InterpreterFrame, every BaselineFrame is either a global frame
// or a function frame. Both global and function frames can optionally
// be "eval frames". The callee token for eval function frames is the
// enclosing function. BaselineFrame::evalScript_ stores the eval script
// itself.
class BaselineFrame
{
  public:
    enum Flags : uint32_t {
        // The frame has a valid return value. See also InterpreterFrame::HAS_RVAL.
        HAS_RVAL         = 1 << 0,

        // A call object has been pushed on the scope chain.
        HAS_CALL_OBJ     = 1 << 2,

        // Frame has an arguments object, argsObj_.
        HAS_ARGS_OBJ     = 1 << 4,

        // See InterpreterFrame::PREV_UP_TO_DATE.
        PREV_UP_TO_DATE  = 1 << 5,

        // Frame has execution observed by a Debugger.
        //
        // See comment above 'isDebuggee' in jscompartment.h for explanation of
        // invariants of debuggee compartments, scripts, and frames.
        DEBUGGEE         = 1 << 6,

        // Eval frame, see the "eval frames" comment.
        EVAL             = 1 << 7,

        // Frame has over-recursed on an early check.
        OVER_RECURSED    = 1 << 9,

        // Frame has a BaselineRecompileInfo stashed in the scratch value
        // slot. See PatchBaselineFramesForDebugMode.
        HAS_DEBUG_MODE_OSR_INFO = 1 << 10,

        // This flag is intended for use whenever the frame is settled on a
        // native code address without a corresponding ICEntry. In this case,
        // the frame contains an explicit bytecode offset for frame iterators.
        //
        // There can also be an override pc if the frame has had its scope chain
        // unwound to a pc during exception handling that is different from its
        // current pc.
        //
        // This flag should never be set when we're executing JIT code.
        HAS_OVERRIDE_PC = 1 << 11,

        // If set, we're handling an exception for this frame. This is set for
        // debug mode OSR sanity checking when it handles corner cases which
        // only arise during exception handling.
        HANDLING_EXCEPTION = 1 << 12,

        // If set, this frame has been on the stack when
        // |js::SavedStacks::saveCurrentStack| was called, and so there is a
        // |js::SavedFrame| object cached for this frame.
        HAS_CACHED_SAVED_FRAME = 1 << 13
    };

  protected: // Silence Clang warning about unused private fields.
    // We need to split the Value into 2 fields of 32 bits, otherwise the C++
    // compiler may add some padding between the fields.

    union {
        struct {
            uint32_t loScratchValue_;
            uint32_t hiScratchValue_;
        };
        BaselineDebugModeOSRInfo* debugModeOSRInfo_;
    };
    uint32_t loReturnValue_;              // If HAS_RVAL, the frame's return value.
    uint32_t hiReturnValue_;
    uint32_t frameSize_;
    JSObject* scopeChain_;                // Scope chain (always initialized).
    JSScript* evalScript_;                // If isEvalFrame(), the current eval script.
    ArgumentsObject* argsObj_;            // If HAS_ARGS_OBJ, the arguments object.
    void* unused;                         // See static assertion re: sizeof, below.
    uint32_t overrideOffset_;             // If HAS_OVERRIDE_PC, the bytecode offset.
    uint32_t flags_;

  public:
    // Distance between the frame pointer and the frame header (return address).
    // This is the old frame pointer saved in the prologue.
    static const uint32_t FramePointerOffset = sizeof(void*);

    bool initForOsr(InterpreterFrame* fp, uint32_t numStackValues);

    uint32_t frameSize() const {
        return frameSize_;
    }
    void setFrameSize(uint32_t frameSize) {
        frameSize_ = frameSize;
    }
    inline uint32_t* addressOfFrameSize() {
        return &frameSize_;
    }
    JSObject* scopeChain() const {
        return scopeChain_;
    }
    void setScopeChain(JSObject* scopeChain) {
        scopeChain_ = scopeChain;
    }
    inline JSObject** addressOfScopeChain() {
        return &scopeChain_;
    }

    inline Value* addressOfScratchValue() {
        return reinterpret_cast<Value*>(&loScratchValue_);
    }

    inline void pushOnScopeChain(ScopeObject& scope);
    inline void popOffScopeChain();
    inline void replaceInnermostScope(ScopeObject& scope);

    inline void popWith(JSContext* cx);

    CalleeToken calleeToken() const {
        uint8_t* pointer = (uint8_t*)this + Size() + offsetOfCalleeToken();
        return *(CalleeToken*)pointer;
    }
    void replaceCalleeToken(CalleeToken token) {
        uint8_t* pointer = (uint8_t*)this + Size() + offsetOfCalleeToken();
        *(CalleeToken*)pointer = token;
    }
    bool isConstructing() const {
        return CalleeTokenIsConstructing(calleeToken());
    }
    JSScript* script() const {
        if (isEvalFrame())
            return evalScript();
        return ScriptFromCalleeToken(calleeToken());
    }
    JSFunction* fun() const {
        return CalleeTokenToFunction(calleeToken());
    }
    JSFunction* maybeFun() const {
        return isFunctionFrame() ? fun() : nullptr;
    }
    JSFunction* callee() const {
        return CalleeTokenToFunction(calleeToken());
    }
    Value calleev() const {
        return ObjectValue(*callee());
    }
    size_t numValueSlots() const {
        size_t size = frameSize();

        MOZ_ASSERT(size >= BaselineFrame::FramePointerOffset + BaselineFrame::Size());
        size -= BaselineFrame::FramePointerOffset + BaselineFrame::Size();

        MOZ_ASSERT((size % sizeof(Value)) == 0);
        return size / sizeof(Value);
    }
    Value* valueSlot(size_t slot) const {
        MOZ_ASSERT(slot < numValueSlots());
        return (Value*)this - (slot + 1);
    }

    Value& unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) const {
        MOZ_ASSERT(i < numFormalArgs());
        MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals() &&
                                     !script()->formalIsAliased(i));
        return argv()[i];
    }

    Value& unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) const {
        MOZ_ASSERT(i < numActualArgs());
        MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
        MOZ_ASSERT_IF(checkAliasing && i < numFormalArgs(), !script()->formalIsAliased(i));
        return argv()[i];
    }

    Value& unaliasedLocal(uint32_t i) const {
        MOZ_ASSERT(i < script()->nfixed());
        return *valueSlot(i);
    }

    unsigned numActualArgs() const {
        return *(size_t*)(reinterpret_cast<const uint8_t*>(this) +
                             BaselineFrame::Size() +
                             offsetOfNumActualArgs());
    }
    unsigned numFormalArgs() const {
        return script()->functionNonDelazifying()->nargs();
    }
    Value& thisArgument() const {
        MOZ_ASSERT(isNonEvalFunctionFrame());
        return *(Value*)(reinterpret_cast<const uint8_t*>(this) +
                         BaselineFrame::Size() +
                         offsetOfThis());
    }
    Value* argv() const {
        return (Value*)(reinterpret_cast<const uint8_t*>(this) +
                         BaselineFrame::Size() +
                         offsetOfArg(0));
    }

  private:
    Value* evalNewTargetAddress() const {
        MOZ_ASSERT(isEvalFrame());
        MOZ_ASSERT(isFunctionFrame());
        return (Value*)(reinterpret_cast<const uint8_t*>(this) +
                        BaselineFrame::Size() +
                        offsetOfEvalNewTarget());
    }

  public:
    Value newTarget() const {
        MOZ_ASSERT(isFunctionFrame());
        if (isEvalFrame())
            return *evalNewTargetAddress();
        if (fun()->isArrow())
            return fun()->getExtendedSlot(FunctionExtended::ARROW_NEWTARGET_SLOT);
        if (isConstructing())
            return *(Value*)(reinterpret_cast<const uint8_t*>(this) +
                             BaselineFrame::Size() +
                             offsetOfArg(Max(numFormalArgs(), numActualArgs())));
        return UndefinedValue();
    }

    bool copyRawFrameSlots(AutoValueVector* vec) const;

    bool hasReturnValue() const {
        return flags_ & HAS_RVAL;
    }
    MutableHandleValue returnValue() {
        if (!hasReturnValue())
            addressOfReturnValue()->setUndefined();
        return MutableHandleValue::fromMarkedLocation(addressOfReturnValue());
    }
    void setReturnValue(const Value& v) {
        returnValue().set(v);
        flags_ |= HAS_RVAL;
    }
    inline Value* addressOfReturnValue() {
        return reinterpret_cast<Value*>(&loReturnValue_);
    }

    bool hasCallObj() const {
        return flags_ & HAS_CALL_OBJ;
    }

    inline CallObject& callObj() const;

    void setFlags(uint32_t flags) {
        flags_ = flags;
    }
    uint32_t* addressOfFlags() {
        return &flags_;
    }

    inline bool pushBlock(JSContext* cx, Handle<StaticBlockObject*> block);
    inline void popBlock(JSContext* cx);
    inline bool freshenBlock(JSContext* cx);

    bool initStrictEvalScopeObjects(JSContext* cx);
    bool initFunctionScopeObjects(JSContext* cx);

    void initArgsObjUnchecked(ArgumentsObject& argsobj) {
        flags_ |= HAS_ARGS_OBJ;
        argsObj_ = &argsobj;
    }
    void initArgsObj(ArgumentsObject& argsobj) {
        MOZ_ASSERT(script()->needsArgsObj());
        initArgsObjUnchecked(argsobj);
    }
    bool hasArgsObj() const {
        return flags_ & HAS_ARGS_OBJ;
    }
    ArgumentsObject& argsObj() const {
        MOZ_ASSERT(hasArgsObj());
        MOZ_ASSERT(script()->needsArgsObj());
        return *argsObj_;
    }

    bool prevUpToDate() const {
        return flags_ & PREV_UP_TO_DATE;
    }
    void setPrevUpToDate() {
        flags_ |= PREV_UP_TO_DATE;
    }
    void unsetPrevUpToDate() {
        flags_ &= ~PREV_UP_TO_DATE;
    }

    bool isDebuggee() const {
        return flags_ & DEBUGGEE;
    }
    void setIsDebuggee() {
        flags_ |= DEBUGGEE;
    }
    inline void unsetIsDebuggee();

    bool isHandlingException() const {
        return flags_ & HANDLING_EXCEPTION;
    }
    void setIsHandlingException() {
        flags_ |= HANDLING_EXCEPTION;
    }
    void unsetIsHandlingException() {
        flags_ &= ~HANDLING_EXCEPTION;
    }

    bool hasCachedSavedFrame() const {
        return flags_ & HAS_CACHED_SAVED_FRAME;
    }
    void setHasCachedSavedFrame() {
        flags_ |= HAS_CACHED_SAVED_FRAME;
    }

    JSScript* evalScript() const {
        MOZ_ASSERT(isEvalFrame());
        return evalScript_;
    }

    bool overRecursed() const {
        return flags_ & OVER_RECURSED;
    }

    void setOverRecursed() {
        flags_ |= OVER_RECURSED;
    }

    BaselineDebugModeOSRInfo* debugModeOSRInfo() {
        MOZ_ASSERT(flags_ & HAS_DEBUG_MODE_OSR_INFO);
        return debugModeOSRInfo_;
    }

    BaselineDebugModeOSRInfo* getDebugModeOSRInfo() {
        if (flags_ & HAS_DEBUG_MODE_OSR_INFO)
            return debugModeOSRInfo();
        return nullptr;
    }

    void setDebugModeOSRInfo(BaselineDebugModeOSRInfo* info) {
        flags_ |= HAS_DEBUG_MODE_OSR_INFO;
        debugModeOSRInfo_ = info;
    }

    void deleteDebugModeOSRInfo();

    // See the HAS_OVERRIDE_PC comment.
    bool hasOverridePc() const {
        return flags_ & HAS_OVERRIDE_PC;
    }

    jsbytecode* overridePc() const {
        MOZ_ASSERT(hasOverridePc());
        return script()->offsetToPC(overrideOffset_);
    }

    jsbytecode* maybeOverridePc() const {
        if (hasOverridePc())
            return overridePc();
        return nullptr;
    }

    void setOverridePc(jsbytecode* pc) {
        flags_ |= HAS_OVERRIDE_PC;
        overrideOffset_ = script()->pcToOffset(pc);
    }

    void clearOverridePc() {
        flags_ &= ~HAS_OVERRIDE_PC;
    }

    void trace(JSTracer* trc, JitFrameIterator& frame);

    bool isFunctionFrame() const {
        return CalleeTokenIsFunction(calleeToken());
    }
    bool isModuleFrame() const {
        return CalleeTokenIsModuleScript(calleeToken());
    }
    bool isGlobalFrame() const {
        return !isFunctionFrame() && !isModuleFrame();
    }
     bool isEvalFrame() const {
        return flags_ & EVAL;
    }
    bool isStrictEvalFrame() const {
        return isEvalFrame() && script()->strict();
    }
    bool isNonStrictEvalFrame() const {
        return isEvalFrame() && !script()->strict();
    }
    bool isNonGlobalEvalFrame() const;
    bool isNonStrictDirectEvalFrame() const {
        return isNonStrictEvalFrame() && isNonGlobalEvalFrame();
    }
    bool isNonEvalFunctionFrame() const {
        return isFunctionFrame() && !isEvalFrame();
    }
    bool isDebuggerEvalFrame() const {
        return false;
    }

    JitFrameLayout* framePrefix() const {
        uint8_t* fp = (uint8_t*)this + Size() + FramePointerOffset;
        return (JitFrameLayout*)fp;
    }

    // Methods below are used by the compiler.
    static size_t offsetOfCalleeToken() {
        return FramePointerOffset + js::jit::JitFrameLayout::offsetOfCalleeToken();
    }
    static size_t offsetOfThis() {
        return FramePointerOffset + js::jit::JitFrameLayout::offsetOfThis();
    }
    static size_t offsetOfEvalNewTarget() {
        return FramePointerOffset + js::jit::JitFrameLayout::offsetOfEvalNewTarget();
    }
    static size_t offsetOfArg(size_t index) {
        return FramePointerOffset + js::jit::JitFrameLayout::offsetOfActualArg(index);
    }
    static size_t offsetOfNumActualArgs() {
        return FramePointerOffset + js::jit::JitFrameLayout::offsetOfNumActualArgs();
    }
    static size_t Size() {
        return sizeof(BaselineFrame);
    }

    // The reverseOffsetOf methods below compute the offset relative to the
    // frame's base pointer. Since the stack grows down, these offsets are
    // negative.
    static int reverseOffsetOfFrameSize() {
        return -int(Size()) + offsetof(BaselineFrame, frameSize_);
    }
    static int reverseOffsetOfScratchValue() {
        return -int(Size()) + offsetof(BaselineFrame, loScratchValue_);
    }
    static int reverseOffsetOfScopeChain() {
        return -int(Size()) + offsetof(BaselineFrame, scopeChain_);
    }
    static int reverseOffsetOfArgsObj() {
        return -int(Size()) + offsetof(BaselineFrame, argsObj_);
    }
    static int reverseOffsetOfFlags() {
        return -int(Size()) + offsetof(BaselineFrame, flags_);
    }
    static int reverseOffsetOfEvalScript() {
        return -int(Size()) + offsetof(BaselineFrame, evalScript_);
    }
    static int reverseOffsetOfReturnValue() {
        return -int(Size()) + offsetof(BaselineFrame, loReturnValue_);
    }
    static int reverseOffsetOfLocal(size_t index) {
        return -int(Size()) - (index + 1) * sizeof(Value);
    }
};

// Ensure the frame is 8-byte aligned (required on ARM).
JS_STATIC_ASSERT(((sizeof(BaselineFrame) + BaselineFrame::FramePointerOffset) % 8) == 0);

} // namespace jit
} // namespace js

#endif /* jit_BaselineFrame_h */
