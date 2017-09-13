/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsscriptinlines_h
#define jsscriptinlines_h

#include "jsscript.h"

#include "asmjs/AsmJSLink.h"
#include "jit/BaselineJIT.h"
#include "jit/IonAnalysis.h"
#include "vm/ScopeObject.h"

#include "jscompartmentinlines.h"

#include "vm/Shape-inl.h"

namespace js {

inline
AliasedFormalIter::AliasedFormalIter(JSScript* script)
  : begin_(script->bindingArray()),
    p_(begin_),
    end_(begin_ + (script->funHasAnyAliasedFormal() ? script->numArgs() : 0)),
    slot_(CallObject::RESERVED_SLOTS)
{
    settle();
}

ScriptCounts::ScriptCounts()
  : pcCounts_(),
    throwCounts_(),
    ionCounts_(nullptr)
{
}

ScriptCounts::ScriptCounts(PCCountsVector&& jumpTargets)
  : pcCounts_(Move(jumpTargets)),
    throwCounts_(),
    ionCounts_(nullptr)
{
}

ScriptCounts::ScriptCounts(ScriptCounts&& src)
  : pcCounts_(Move(src.pcCounts_)),
    throwCounts_(Move(src.throwCounts_)),
    ionCounts_(Move(src.ionCounts_))
{
    src.ionCounts_ = nullptr;
}

ScriptCounts&
ScriptCounts::operator=(ScriptCounts&& src)
{
    pcCounts_ = Move(src.pcCounts_);
    throwCounts_ = Move(src.throwCounts_);
    ionCounts_ = Move(src.ionCounts_);
    src.ionCounts_ = nullptr;
    return *this;
}

ScriptCounts::~ScriptCounts()
{
    js_delete(ionCounts_);
}

ScriptAndCounts::ScriptAndCounts(JSScript* script)
  : script(script),
    scriptCounts()
{
    script->releaseScriptCounts(&scriptCounts);
}

ScriptAndCounts::ScriptAndCounts(ScriptAndCounts&& sac)
  : script(Move(sac.script)),
    scriptCounts(Move(sac.scriptCounts))
{
}

void
SetFrameArgumentsObject(JSContext* cx, AbstractFramePtr frame,
                        HandleScript script, JSObject* argsobj);

inline JSFunction*
LazyScript::functionDelazifying(JSContext* cx) const
{
    if (function_ && !function_->getOrCreateScript(cx))
        return nullptr;
    return function_;
}

} // namespace js

inline JSFunction*
JSScript::functionDelazifying() const
{
    if (function_ && function_->isInterpretedLazy()) {
        function_->setUnlazifiedScript(const_cast<JSScript*>(this));
        // If this script has a LazyScript, make sure the LazyScript has a
        // reference to the script when delazifying its canonical function.
        if (lazyScript && !lazyScript->maybeScript())
            lazyScript->initScript(const_cast<JSScript*>(this));
    }
    return function_;
}

inline void
JSScript::setFunction(JSFunction* fun)
{
    MOZ_ASSERT(!function_ && !module_);
    MOZ_ASSERT(fun->isTenured());
    function_ = fun;
}

inline void
JSScript::setModule(js::ModuleObject* module)
{
    MOZ_ASSERT(!function_ && !module_);
    module_ = module;
}

inline void
JSScript::ensureNonLazyCanonicalFunction(JSContext* cx)
{
    // Infallibly delazify the canonical script.
    if (function_ && function_->isInterpretedLazy())
        functionDelazifying();
}

inline JSFunction*
JSScript::getFunction(size_t index)
{
    JSFunction* fun = &getObject(index)->as<JSFunction>();
    MOZ_ASSERT_IF(fun->isNative(), IsAsmJSModuleNative(fun->native()));
    return fun;
}

inline JSFunction*
JSScript::getCallerFunction()
{
    MOZ_ASSERT(savedCallerFun());
    return getFunction(0);
}

inline JSFunction*
JSScript::functionOrCallerFunction()
{
    if (functionNonDelazifying())
        return functionNonDelazifying();
    if (savedCallerFun())
        return getCallerFunction();
    return nullptr;
}

inline js::RegExpObject*
JSScript::getRegExp(size_t index)
{
    js::ObjectArray* arr = regexps();
    MOZ_ASSERT(uint32_t(index) < arr->length);
    JSObject* obj = arr->vector[index];
    MOZ_ASSERT(obj->is<js::RegExpObject>());
    return (js::RegExpObject*) obj;
}

inline js::RegExpObject*
JSScript::getRegExp(jsbytecode* pc)
{
    MOZ_ASSERT(containsPC(pc) && containsPC(pc + sizeof(uint32_t)));
    return getRegExp(GET_UINT32_INDEX(pc));
}

inline js::GlobalObject&
JSScript::global() const
{
    /*
     * A JSScript always marks its compartment's global (via bindings) so we
     * can assert that maybeGlobal is non-null here.
     */
    return *compartment()->maybeGlobal();
}

inline JSPrincipals*
JSScript::principals()
{
    return compartment()->principals();
}

inline void
JSScript::setBaselineScript(JSContext* maybecx, js::jit::BaselineScript* baselineScript)
{
    if (hasBaselineScript())
        js::jit::BaselineScript::writeBarrierPre(zone(), baseline);
    MOZ_ASSERT(!hasIonScript());
    baseline = baselineScript;
    resetWarmUpResetCounter();
    updateBaselineOrIonRaw(maybecx);
}

inline bool
JSScript::ensureHasAnalyzedArgsUsage(JSContext* cx)
{
    if (analyzedArgsUsage())
        return true;
    return js::jit::AnalyzeArgumentsUsage(cx, this);
}

inline bool
JSScript::isDebuggee() const
{
    return compartment_->debuggerObservesAllExecution() || hasDebugScript_;
}

#endif /* jsscriptinlines_h */
