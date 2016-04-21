/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RematerializedFrame_h
#define jit_RematerializedFrame_h

#include "jsfun.h"

#include "jit/JitFrameIterator.h"
#include "jit/JitFrames.h"

#include "vm/Stack.h"

namespace js {
namespace jit {

//
// An optimized frame that has been rematerialized with values read out of
// Snapshots.
//
class RematerializedFrame
{
    // See DebugScopes::updateLiveScopes.
    bool prevUpToDate_;

    // Propagated to the Baseline frame once this is popped.
    bool isDebuggee_;

    // Has a call object been pushed?
    bool hasCallObj_;

    // Is this frame constructing?
    bool isConstructing_;

    // If true, this frame has been on the stack when
    // |js::SavedStacks::saveCurrentStack| was called, and so there is a
    // |js::SavedFrame| object cached for this frame.
    bool hasCachedSavedFrame_;

    // The fp of the top frame associated with this possibly inlined frame.
    uint8_t* top_;

    // The bytecode at the time of rematerialization.
    jsbytecode* pc_;

    size_t frameNo_;
    unsigned numActualArgs_;

    JSScript* script_;
    JSObject* scopeChain_;
    JSFunction* callee_;
    ArgumentsObject* argsObj_;

    Value returnValue_;
    Value thisArgument_;
    Value slots_[1];

    RematerializedFrame(JSContext* cx, uint8_t* top, unsigned numActualArgs,
                        InlineFrameIterator& iter, MaybeReadFallback& fallback);

  public:
    static RematerializedFrame* New(JSContext* cx, uint8_t* top, InlineFrameIterator& iter,
                                    MaybeReadFallback& fallback);

    // Rematerialize all remaining frames pointed to by |iter| into |frames|
    // in older-to-younger order, e.g., frames[0] is the oldest frame.
    static bool RematerializeInlineFrames(JSContext* cx, uint8_t* top,
                                          InlineFrameIterator& iter,
                                          MaybeReadFallback& fallback,
                                          Vector<RematerializedFrame*>& frames);

    // Free a vector of RematerializedFrames; takes care to call the
    // destructor. Also clears the vector.
    static void FreeInVector(Vector<RematerializedFrame*>& frames);

    // Mark a vector of RematerializedFrames.
    static void MarkInVector(JSTracer* trc, Vector<RematerializedFrame*>& frames);

    bool prevUpToDate() const {
        return prevUpToDate_;
    }
    void setPrevUpToDate() {
        prevUpToDate_ = true;
    }
    void unsetPrevUpToDate() {
        prevUpToDate_ = false;
    }

    bool isDebuggee() const {
        return isDebuggee_;
    }
    void setIsDebuggee() {
        isDebuggee_ = true;
    }
    void unsetIsDebuggee() {
        MOZ_ASSERT(!script()->isDebuggee());
        isDebuggee_ = false;
    }

    uint8_t* top() const {
        return top_;
    }
    JSScript* outerScript() const {
        JitFrameLayout* jsFrame = (JitFrameLayout*)top_;
        return ScriptFromCalleeToken(jsFrame->calleeToken());
    }
    jsbytecode* pc() const {
        return pc_;
    }
    size_t frameNo() const {
        return frameNo_;
    }
    bool inlined() const {
        return frameNo_ > 0;
    }

    JSObject* scopeChain() const {
        return scopeChain_;
    }
    void pushOnScopeChain(ScopeObject& scope);
    bool initFunctionScopeObjects(JSContext* cx);

    bool hasCallObj() const {
        MOZ_ASSERT(fun()->needsCallObject());
        return hasCallObj_;
    }
    CallObject& callObj() const;

    bool hasArgsObj() const {
        return !!argsObj_;
    }
    ArgumentsObject& argsObj() const {
        MOZ_ASSERT(hasArgsObj());
        MOZ_ASSERT(script()->needsArgsObj());
        return *argsObj_;
    }

    bool isFunctionFrame() const {
        return !!script_->functionNonDelazifying();
    }
    bool isModuleFrame() const {
        return !!script_->module();
    }
    bool isGlobalFrame() const {
        return !isFunctionFrame() && !isModuleFrame();
    }
    bool isNonEvalFunctionFrame() const {
        // Ion doesn't support eval frames.
        return isFunctionFrame();
    }

    JSScript* script() const {
        return script_;
    }
    JSFunction* fun() const {
        MOZ_ASSERT(isFunctionFrame());
        return script_->functionNonDelazifying();
    }
    JSFunction* maybeFun() const {
        return isFunctionFrame() ? fun() : nullptr;
    }
    JSFunction* callee() const {
        MOZ_ASSERT(isFunctionFrame());
        return callee_;
    }
    Value calleev() const {
        return ObjectValue(*callee());
    }
    Value& thisArgument() {
        return thisArgument_;
    }

    bool isConstructing() const {
        return isConstructing_;
    }

    bool hasCachedSavedFrame() const {
        return hasCachedSavedFrame_;
    }

    void setHasCachedSavedFrame() {
        hasCachedSavedFrame_ = true;
    }

    unsigned numFormalArgs() const {
        return maybeFun() ? fun()->nargs() : 0;
    }
    unsigned numActualArgs() const {
        return numActualArgs_;
    }

    Value* argv() {
        return slots_;
    }
    Value* locals() {
        return slots_ + numActualArgs_ + isConstructing_;
    }

    Value& unaliasedLocal(unsigned i) {
        MOZ_ASSERT(i < script()->nfixed());
        return locals()[i];
    }
    Value& unaliasedFormal(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) {
        MOZ_ASSERT(i < numFormalArgs());
        MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals() &&
                                     !script()->formalIsAliased(i));
        return argv()[i];
    }
    Value& unaliasedActual(unsigned i, MaybeCheckAliasing checkAliasing = CHECK_ALIASING) {
        MOZ_ASSERT(i < numActualArgs());
        MOZ_ASSERT_IF(checkAliasing, !script()->argsObjAliasesFormals());
        MOZ_ASSERT_IF(checkAliasing && i < numFormalArgs(), !script()->formalIsAliased(i));
        return argv()[i];
    }

    Value newTarget() {
        MOZ_ASSERT(isFunctionFrame());
        if (callee()->isArrow())
            return callee()->getExtendedSlot(FunctionExtended::ARROW_NEWTARGET_SLOT);
        if (isConstructing())
            return argv()[numActualArgs()];
        return UndefinedValue();
    }

    Value returnValue() const {
        return returnValue_;
    }

    void mark(JSTracer* trc);
    void dump();
};

} // namespace jit
} // namespace js

#endif // jit_RematerializedFrame_h
