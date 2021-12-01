/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Eval.h"

#include "mozilla/HashFunctions.h"
#include "mozilla/Range.h"

#include "frontend/BytecodeCompiler.h"
#include "gc/HashUtil.h"
#include "vm/Debugger.h"
#include "vm/GlobalObject.h"
#include "vm/JSContext.h"
#include "vm/JSONParser.h"

#include "vm/Interpreter-inl.h"

using namespace js;

using mozilla::AddToHash;
using mozilla::HashString;
using mozilla::RangedPtr;

using JS::AutoCheckCannotGC;

// We should be able to assert this for *any* fp->environmentChain().
static void
AssertInnerizedEnvironmentChain(JSContext* cx, JSObject& env)
{
#ifdef DEBUG
    RootedObject obj(cx);
    for (obj = &env; obj; obj = obj->enclosingEnvironment())
        MOZ_ASSERT(!IsWindowProxy(obj));
#endif
}

static bool
IsEvalCacheCandidate(JSScript* script)
{
    // Make sure there are no inner objects which might use the wrong parent
    // and/or call scope by reusing the previous eval's script.
    return script->isDirectEvalInFunction() &&
           !script->hasSingletons() &&
           !script->hasObjects();
}

/* static */ HashNumber
EvalCacheHashPolicy::hash(const EvalCacheLookup& l)
{
    AutoCheckCannotGC nogc;
    uint32_t hash = l.str->hasLatin1Chars()
                    ? HashString(l.str->latin1Chars(nogc), l.str->length())
                    : HashString(l.str->twoByteChars(nogc), l.str->length());
    return AddToHash(hash, l.callerScript.get(), l.pc);
}

/* static */ bool
EvalCacheHashPolicy::match(const EvalCacheEntry& cacheEntry, const EvalCacheLookup& l)
{
    MOZ_ASSERT(IsEvalCacheCandidate(cacheEntry.script));

    return EqualStrings(cacheEntry.str, l.str) &&
           cacheEntry.callerScript == l.callerScript &&
           cacheEntry.pc == l.pc;
}

// Add the script to the eval cache when EvalKernel is finished
class EvalScriptGuard
{
    JSContext* cx_;
    Rooted<JSScript*> script_;

    /* These fields are only valid if lookup_.str is non-nullptr. */
    EvalCacheLookup lookup_;
    mozilla::Maybe<DependentAddPtr<EvalCache>> p_;

    RootedLinearString lookupStr_;

  public:
    explicit EvalScriptGuard(JSContext* cx)
        : cx_(cx), script_(cx), lookup_(cx), lookupStr_(cx) {}

    ~EvalScriptGuard() {
        if (script_ && !cx_->isExceptionPending()) {
            script_->cacheForEval();
            EvalCacheEntry cacheEntry = {lookupStr_, script_, lookup_.callerScript, lookup_.pc};
            lookup_.str = lookupStr_;
            if (lookup_.str && IsEvalCacheCandidate(script_)) {
                // Ignore failure to add cache entry.
                if (!p_->add(cx_, cx_->caches().evalCache, lookup_, cacheEntry))
                    cx_->recoverFromOutOfMemory();
            }
        }
    }

    void lookupInEvalCache(JSLinearString* str, JSScript* callerScript, jsbytecode* pc)
    {
        lookupStr_ = str;
        lookup_.str = str;
        lookup_.callerScript = callerScript;
        lookup_.pc = pc;
        p_.emplace(cx_, cx_->caches().evalCache, lookup_);
        if (*p_) {
            script_ = (*p_)->script;
            p_->remove(cx_, cx_->caches().evalCache, lookup_);
            script_->uncacheForEval();
        }
    }

    void setNewScript(JSScript* script) {
        // JSScript::initFromEmitter has already called js_CallNewScriptHook.
        MOZ_ASSERT(!script_ && script);
        script_ = script;
        script_->setActiveEval();
    }

    bool foundScript() {
        return !!script_;
    }

    HandleScript script() {
        MOZ_ASSERT(script_);
        return script_;
    }
};

enum EvalJSONResult {
    EvalJSON_Failure,
    EvalJSON_Success,
    EvalJSON_NotJSON
};

template <typename CharT>
static bool
EvalStringMightBeJSON(const mozilla::Range<const CharT> chars)
{
    // If the eval string starts with '(' or '[' and ends with ')' or ']', it may be JSON.
    // Try the JSON parser first because it's much faster.  If the eval string
    // isn't JSON, JSON parsing will probably fail quickly, so little time
    // will be lost.
    size_t length = chars.length();
    if (length > 2 &&
        ((chars[0] == '[' && chars[length - 1] == ']') ||
         (chars[0] == '(' && chars[length - 1] == ')')))
    {
        // Remarkably, JavaScript syntax is not a superset of JSON syntax:
        // strings in JavaScript cannot contain the Unicode line and paragraph
        // terminator characters U+2028 and U+2029, but strings in JSON can.
        // Rather than force the JSON parser to handle this quirk when used by
        // eval, we simply don't use the JSON parser when either character
        // appears in the provided string.  See bug 657367.
        if (sizeof(CharT) > 1) {
            for (RangedPtr<const CharT> cp = chars.begin() + 1, end = chars.end() - 1;
                 cp < end;
                 cp++)
            {
                char16_t c = *cp;
                if (c == 0x2028 || c == 0x2029)
                    return false;
            }
        }

        return true;
    }
    return false;
}

template <typename CharT>
static EvalJSONResult
ParseEvalStringAsJSON(JSContext* cx, const mozilla::Range<const CharT> chars, MutableHandleValue rval)
{
    size_t len = chars.length();
    MOZ_ASSERT((chars[0] == '(' && chars[len - 1] == ')') ||
               (chars[0] == '[' && chars[len - 1] == ']'));

    auto jsonChars = (chars[0] == '[')
                     ? chars
                     : mozilla::Range<const CharT>(chars.begin().get() + 1U, len - 2);

    Rooted<JSONParser<CharT>> parser(cx, JSONParser<CharT>(cx, jsonChars, JSONParserBase::NoError));
    if (!parser.parse(rval))
        return EvalJSON_Failure;

    return rval.isUndefined() ? EvalJSON_NotJSON : EvalJSON_Success;
}

static EvalJSONResult
TryEvalJSON(JSContext* cx, JSLinearString* str, MutableHandleValue rval)
{
    if (str->hasLatin1Chars()) {
        AutoCheckCannotGC nogc;
        if (!EvalStringMightBeJSON(str->latin1Range(nogc)))
            return EvalJSON_NotJSON;
    } else {
        AutoCheckCannotGC nogc;
        if (!EvalStringMightBeJSON(str->twoByteRange(nogc)))
            return EvalJSON_NotJSON;
    }

    AutoStableStringChars linearChars(cx);
    if (!linearChars.init(cx, str))
        return EvalJSON_Failure;

    return linearChars.isLatin1()
           ? ParseEvalStringAsJSON(cx, linearChars.latin1Range(), rval)
           : ParseEvalStringAsJSON(cx, linearChars.twoByteRange(), rval);
}

enum EvalType { DIRECT_EVAL, INDIRECT_EVAL };

// Common code implementing direct and indirect eval.
//
// Evaluate call.argv[2], if it is a string, in the context of the given calling
// frame, with the provided scope chain, with the semantics of either a direct
// or indirect eval (see ES5 10.4.2).  If this is an indirect eval, env
// must be a global object.
//
// On success, store the completion value in call.rval and return true.
static bool
EvalKernel(JSContext* cx, HandleValue v, EvalType evalType, AbstractFramePtr caller,
           HandleObject env, jsbytecode* pc, MutableHandleValue vp)
{
    MOZ_ASSERT((evalType == INDIRECT_EVAL) == !caller);
    MOZ_ASSERT((evalType == INDIRECT_EVAL) == !pc);
    MOZ_ASSERT_IF(evalType == INDIRECT_EVAL, IsGlobalLexicalEnvironment(env));
    AssertInnerizedEnvironmentChain(cx, *env);

    Rooted<GlobalObject*> envGlobal(cx, &env->global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, envGlobal)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_EVAL);
        return false;
    }

    // ES5 15.1.2.1 step 1.
    if (!v.isString()) {
        vp.set(v);
        return true;
    }
    RootedString str(cx, v.toString());

    // ES5 15.1.2.1 steps 2-8.

    // Per ES5, indirect eval runs in the global scope. (eval is specified this
    // way so that the compiler can make assumptions about what bindings may or
    // may not exist in the current frame if it doesn't see 'eval'.)
    MOZ_ASSERT_IF(evalType != DIRECT_EVAL,
                  cx->global() == &env->as<LexicalEnvironmentObject>().global());

    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return false;

    RootedScript callerScript(cx, caller ? caller.script() : nullptr);
    EvalJSONResult ejr = TryEvalJSON(cx, linearStr, vp);
    if (ejr != EvalJSON_NotJSON)
        return ejr == EvalJSON_Success;

    EvalScriptGuard esg(cx);

    if (evalType == DIRECT_EVAL && caller.isFunctionFrame())
        esg.lookupInEvalCache(linearStr, callerScript, pc);

    if (!esg.foundScript()) {
        RootedScript maybeScript(cx);
        unsigned lineno;
        const char* filename;
        bool mutedErrors;
        uint32_t pcOffset;
        DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno, &pcOffset,
                                             &mutedErrors,
                                             evalType == DIRECT_EVAL
                                             ? CALLED_FROM_JSOP_EVAL
                                             : NOT_CALLED_FROM_JSOP_EVAL);

        const char* introducerFilename = filename;
        if (maybeScript && maybeScript->scriptSource()->introducerFilename())
            introducerFilename = maybeScript->scriptSource()->introducerFilename();

        RootedScope enclosing(cx);
        if (evalType == DIRECT_EVAL)
            enclosing = callerScript->innermostScope(pc);
        else
            enclosing = &cx->global()->emptyGlobalScope();

        CompileOptions options(cx);
        options.setIsRunOnce(true)
               .setNoScriptRval(false)
               .setMutedErrors(mutedErrors)
               .maybeMakeStrictMode(evalType == DIRECT_EVAL && IsStrictEvalPC(pc));

        if (introducerFilename) {
            options.setFileAndLine(filename, 1);
            options.setIntroductionInfo(introducerFilename, "eval", lineno, maybeScript, pcOffset);
        } else {
            options.setFileAndLine("eval", 1);
            options.setIntroductionType("eval");
        }

        AutoStableStringChars linearChars(cx);
        if (!linearChars.initTwoByte(cx, linearStr))
            return false;

        const char16_t* chars = linearChars.twoByteRange().begin().get();
        SourceBufferHolder::Ownership ownership = linearChars.maybeGiveOwnershipToCaller()
                                                  ? SourceBufferHolder::GiveOwnership
                                                  : SourceBufferHolder::NoOwnership;
        SourceBufferHolder srcBuf(chars, linearStr->length(), ownership);
        JSScript* compiled = frontend::CompileEvalScript(cx, cx->tempLifoAlloc(),
                                                         env, enclosing,
                                                         options, srcBuf);
        if (!compiled)
            return false;

        esg.setNewScript(compiled);
    }

    // Look up the newTarget from the frame iterator.
    Value newTargetVal = NullValue();
    return ExecuteKernel(cx, esg.script(), *env, newTargetVal,
                         NullFramePtr() /* evalInFrame */, vp.address());
}

bool
js::DirectEvalStringFromIon(JSContext* cx,
                            HandleObject env, HandleScript callerScript,
                            HandleValue newTargetValue, HandleString str,
                            jsbytecode* pc, MutableHandleValue vp)
{
    AssertInnerizedEnvironmentChain(cx, *env);

    Rooted<GlobalObject*> envGlobal(cx, &env->global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, envGlobal)) {
        JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_EVAL);
        return false;
    }

    // ES5 15.1.2.1 steps 2-8.

    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return false;

    EvalJSONResult ejr = TryEvalJSON(cx, linearStr, vp);
    if (ejr != EvalJSON_NotJSON)
        return ejr == EvalJSON_Success;

    EvalScriptGuard esg(cx);

    esg.lookupInEvalCache(linearStr, callerScript, pc);

    if (!esg.foundScript()) {
        RootedScript maybeScript(cx);
        const char* filename;
        unsigned lineno;
        bool mutedErrors;
        uint32_t pcOffset;
        DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno, &pcOffset,
                                             &mutedErrors, CALLED_FROM_JSOP_EVAL);

        const char* introducerFilename = filename;
        if (maybeScript && maybeScript->scriptSource()->introducerFilename())
            introducerFilename = maybeScript->scriptSource()->introducerFilename();

        RootedScope enclosing(cx, callerScript->innermostScope(pc));

        CompileOptions options(cx);
        options.setIsRunOnce(true)
               .setNoScriptRval(false)
               .setMutedErrors(mutedErrors)
               .maybeMakeStrictMode(IsStrictEvalPC(pc));

        if (introducerFilename) {
            options.setFileAndLine(filename, 1);
            options.setIntroductionInfo(introducerFilename, "eval", lineno, maybeScript, pcOffset);
        } else {
            options.setFileAndLine("eval", 1);
            options.setIntroductionType("eval");
        }

        AutoStableStringChars linearChars(cx);
        if (!linearChars.initTwoByte(cx, linearStr))
            return false;

        const char16_t* chars = linearChars.twoByteRange().begin().get();
        SourceBufferHolder::Ownership ownership = linearChars.maybeGiveOwnershipToCaller()
                                                  ? SourceBufferHolder::GiveOwnership
                                                  : SourceBufferHolder::NoOwnership;
        SourceBufferHolder srcBuf(chars, linearStr->length(), ownership);
        JSScript* compiled = frontend::CompileEvalScript(cx, cx->tempLifoAlloc(),
                                                         env, enclosing,
                                                         options, srcBuf);
        if (!compiled)
            return false;

        esg.setNewScript(compiled);
    }

    return ExecuteKernel(cx, esg.script(), *env, newTargetValue,
                         NullFramePtr() /* evalInFrame */, vp.address());
}

bool
js::IndirectEval(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);

    Rooted<GlobalObject*> global(cx, &args.callee().global());
    RootedObject globalLexical(cx, &global->lexicalEnvironment());

    // Note we'll just pass |undefined| here, then return it directly (or throw
    // if runtime codegen is disabled), if no argument is provided.
    return EvalKernel(cx, args.get(0), INDIRECT_EVAL, NullFramePtr(), globalLexical, nullptr,
                      args.rval());
}

bool
js::DirectEval(JSContext* cx, HandleValue v, MutableHandleValue vp)
{
    // Direct eval can assume it was called from an interpreted or baseline frame.
    ScriptFrameIter iter(cx);
    AbstractFramePtr caller = iter.abstractFramePtr();

    MOZ_ASSERT(JSOp(*iter.pc()) == JSOP_EVAL ||
               JSOp(*iter.pc()) == JSOP_STRICTEVAL ||
               JSOp(*iter.pc()) == JSOP_SPREADEVAL ||
               JSOp(*iter.pc()) == JSOP_STRICTSPREADEVAL);
    MOZ_ASSERT(caller.compartment() == caller.script()->compartment());

    RootedObject envChain(cx, caller.environmentChain());
    return EvalKernel(cx, v, DIRECT_EVAL, caller, envChain, iter.pc(), vp);
}

bool
js::IsAnyBuiltinEval(JSFunction* fun)
{
    return fun->maybeNative() == IndirectEval;
}

static bool
ExecuteInExtensibleLexicalEnvironment(JSContext* cx, HandleScript scriptArg, HandleObject env)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, env);
    MOZ_ASSERT(IsExtensibleLexicalEnvironment(env));
    MOZ_RELEASE_ASSERT(scriptArg->hasNonSyntacticScope());

    RootedScript script(cx, scriptArg);
    if (script->compartment() != cx->compartment()) {
        script = CloneGlobalScript(cx, ScopeKind::NonSyntactic, script);
        if (!script)
            return false;

        Debugger::onNewScript(cx, script);
    }

    RootedValue rval(cx);
    return ExecuteKernel(cx, script, *env, UndefinedValue(),
                         NullFramePtr() /* evalInFrame */, rval.address());
}

JS_FRIEND_API(bool)
js::ExecuteInGlobalAndReturnScope(JSContext* cx, HandleObject global, HandleScript scriptArg,
                                  MutableHandleObject envArg)
{
    RootedObject varEnv(cx, NonSyntacticVariablesObject::create(cx));
    if (!varEnv)
        return false;

    // Create lexical environment with |this| == global.
    // NOTE: This is required behavior for Gecko FrameScriptLoader
    RootedObject lexEnv(cx, LexicalEnvironmentObject::createNonSyntactic(cx, varEnv, global));
    if (!lexEnv)
        return false;

    if (!ExecuteInExtensibleLexicalEnvironment(cx, scriptArg, lexEnv))
        return false;

    envArg.set(lexEnv);
    return true;
}

JS_FRIEND_API(JSObject*)
js::NewJSMEnvironment(JSContext* cx)
{
    RootedObject varEnv(cx, NonSyntacticVariablesObject::create(cx));
    if (!varEnv)
        return nullptr;

    // Force LexicalEnvironmentObject to be created
    MOZ_ASSERT(!cx->compartment()->getNonSyntacticLexicalEnvironment(varEnv));
    if (!cx->compartment()->getOrCreateNonSyntacticLexicalEnvironment(cx, varEnv))
        return nullptr;

    return varEnv;
}

JS_FRIEND_API(bool)
js::ExecuteInJSMEnvironment(JSContext* cx, HandleScript scriptArg, HandleObject varEnv)
{
    AutoObjectVector emptyChain(cx);
    return ExecuteInJSMEnvironment(cx, scriptArg, varEnv, emptyChain);
}

JS_FRIEND_API(bool)
js::ExecuteInJSMEnvironment(JSContext* cx, HandleScript scriptArg, HandleObject varEnv,
                            AutoObjectVector& targetObj)
{
    assertSameCompartment(cx, varEnv);
    MOZ_ASSERT(cx->compartment()->getNonSyntacticLexicalEnvironment(varEnv));
    MOZ_DIAGNOSTIC_ASSERT(scriptArg->noScriptRval());

    RootedObject env(cx, JS_ExtensibleLexicalEnvironment(varEnv));

    // If the Gecko subscript loader specifies target objects, we need to add
    // them to the environment. These are added after the NSVO environment.
    if (!targetObj.empty()) {
        // The environment chain will be as follows:
        //      GlobalObject / BackstagePass
        //      LexicalEnvironmentObject[this=global]
        //      NonSyntacticVariablesObject (the JSMEnvironment)
        //      LexicalEnvironmentObject[this=nsvo]
        //      WithEnvironmentObject[target=targetObj]
        //      LexicalEnvironmentObject[this=targetObj] (*)
        //
        //  (*) This environment intentionally intercepts JSOP_GLOBALTHIS, but
        //  not JSOP_FUNCTIONTHIS (which instead will fallback to the NSVO). I
        //  don't make the rules, I just record them.

        // Wrap the target objects in WithEnvironments.
        if (!js::CreateObjectsForEnvironmentChain(cx, targetObj, env, &env))
            return false;

        // See CreateNonSyntacticEnvironmentChain
        if (!JSObject::setQualifiedVarObj(cx, env))
            return false;

        // Create an extensible LexicalEnvironmentObject for target object
        env = cx->compartment()->getOrCreateNonSyntacticLexicalEnvironment(cx, env);
        if (!env)
            return false;
    }

    return ExecuteInExtensibleLexicalEnvironment(cx, scriptArg, env);
}

JS_FRIEND_API(JSObject*)
js::GetJSMEnvironmentOfScriptedCaller(JSContext* cx)
{
    FrameIter iter(cx);
    if (iter.done())
        return nullptr;

    // WASM frames don't always provide their environment, but we also shouldn't
    // expect to see any calling into here.
    MOZ_RELEASE_ASSERT(!iter.isWasm());

    RootedObject env(cx, iter.environmentChain(cx));
    while (env && !env->is<NonSyntacticVariablesObject>())
        env = env->enclosingEnvironment();

    return env;
}

JS_FRIEND_API(bool)
js::IsJSMEnvironment(JSObject* obj)
{
    // NOTE: This also returns true if the NonSyntacticVariablesObject was
    // created for reasons other than the JSM loader.
    return obj->is<NonSyntacticVariablesObject>();
}
