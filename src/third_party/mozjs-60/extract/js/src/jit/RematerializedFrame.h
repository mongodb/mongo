/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RematerializedFrame_h
#define jit_RematerializedFrame_h

#include <algorithm>

#include "jit/JitFrames.h"
#include "jit/JSJitFrameIter.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSFunction.h"
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

    // Has an initial environment has been pushed on the environment chain for
    // function frames that need a CallObject or eval frames that need a
    // VarEnvironmentObject?
    bool hasInitialEnv_;

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
    JSObject* envChain_;
    JSFunction* callee_;
    ArgumentsObject* argsObj_;

    Value returnValue_;
    Value thisArgument_;
    Value newTarget_;
    Value slots_[1];

    RematerializedFrame(JSContext* cx, uint8_t* top, unsigned numActualArgs,
                        InlineFrameIterator& iter, MaybeReadFallback& fallback);

  public:
    static RematerializedFrame* New(JSContext* cx, uint8_t* top, InlineFrameIterator& iter,
                                    MaybeReadFallback& fallback);

    // Rematerialize all remaining frames pointed to by |iter| into |frames|
    // in older-to-younger order, e.g., frames[0] is the oldest frame.
    static MOZ_MUST_USE bool RematerializeInlineFrames(JSContext* cx, uint8_t* top,
                                                       InlineFrameIterator& iter,
                                                       MaybeReadFallback& fallback,
                                                       GCVector<RematerializedFrame*>& frames);

    // Free a vector of RematerializedFrames; takes care to call the
    // destructor. Also clears the vector.
    static void FreeInVector(GCVector<RematerializedFrame*>& frames);

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

    JSObject* environmentChain() const {
        return envChain_;
    }

    template <typename SpecificEnvironment>
    void pushOnEnvironmentChain(SpecificEnvironment& env) {
        MOZ_ASSERT(*environmentChain() == env.enclosingEnvironment());
        envChain_ = &env;
        if (IsFrameInitialEnvironment(this, env))
            hasInitialEnv_ = true;
    }

    template <typename SpecificEnvironment>
    void popOffEnvironmentChain() {
        MOZ_ASSERT(envChain_->is<SpecificEnvironment>());
        envChain_ = &envChain_->as<SpecificEnvironment>().enclosingEnvironment();
    }

    MOZ_MUST_USE bool initFunctionEnvironmentObjects(JSContext* cx);
    MOZ_MUST_USE bool pushVarEnvironment(JSContext* cx, HandleScope scope);

    bool hasInitialEnvironment() const {
        return hasInitialEnv_;
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
    bool isGlobalFrame() const {
        return script_->isGlobalCode();
    }
    bool isModuleFrame() const {
        return script_->module();
    }

    JSScript* script() const {
        return script_;
    }
    JSFunction* callee() const {
        MOZ_ASSERT(isFunctionFrame());
        MOZ_ASSERT(callee_);
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
        return isFunctionFrame() ? callee()->nargs() : 0;
    }
    unsigned numActualArgs() const {
        return numActualArgs_;
    }
    unsigned numArgSlots() const {
        return (std::max)(numFormalArgs(), numActualArgs());
    }

    Value* argv() {
        return slots_;
    }
    Value* locals() {
        return slots_ + numArgSlots();
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
        MOZ_ASSERT_IF(!isConstructing(), newTarget_.isUndefined());
        return newTarget_;
    }

    void setReturnValue(const Value& value) {
        returnValue_ = value;
    }

    Value& returnValue() {
        return returnValue_;
    }

    void trace(JSTracer* trc);
    void dump();
};

} // namespace jit
} // namespace js

namespace JS {

template <>
struct MapTypeToRootKind<js::jit::RematerializedFrame*>
{
    static const RootKind kind = RootKind::Traceable;
};

template <>
struct GCPolicy<js::jit::RematerializedFrame*>
  : public NonGCPointerPolicy<js::jit::RematerializedFrame*>
{};

} // namespace JS

#endif // jit_RematerializedFrame_h
