/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Eval.h"

#include "mozilla/HashFunctions.h"
#include "mozilla/Range.h"

#include "jscntxt.h"

#include "frontend/BytecodeCompiler.h"
#include "vm/Debugger.h"
#include "vm/GlobalObject.h"
#include "vm/JSONParser.h"

#include "vm/Interpreter-inl.h"

using namespace js;

using mozilla::AddToHash;
using mozilla::HashString;
using mozilla::RangedPtr;

using JS::AutoCheckCannotGC;

// We should be able to assert this for *any* fp->scopeChain().
static void
AssertInnerizedScopeChain(JSContext* cx, JSObject& scopeobj)
{
#ifdef DEBUG
    RootedObject obj(cx);
    for (obj = &scopeobj; obj; obj = obj->enclosingScope())
        MOZ_ASSERT(!IsWindowProxy(obj));
#endif
}

static bool
IsEvalCacheCandidate(JSScript* script)
{
    // Make sure there are no inner objects which might use the wrong parent
    // and/or call scope by reusing the previous eval's script. Skip the
    // script's first object, which entrains the eval's scope.
    return script->savedCallerFun() &&
           !script->hasSingletons() &&
           script->objects()->length == 1 &&
           !script->hasRegexps();
}

/* static */ HashNumber
EvalCacheHashPolicy::hash(const EvalCacheLookup& l)
{
    AutoCheckCannotGC nogc;
    uint32_t hash = l.str->hasLatin1Chars()
                    ? HashString(l.str->latin1Chars(nogc), l.str->length())
                    : HashString(l.str->twoByteChars(nogc), l.str->length());
    return AddToHash(hash, l.callerScript.get(), l.version, l.pc);
}

/* static */ bool
EvalCacheHashPolicy::match(const EvalCacheEntry& cacheEntry, const EvalCacheLookup& l)
{
    JSScript* script = cacheEntry.script;

    MOZ_ASSERT(IsEvalCacheCandidate(script));

    return EqualStrings(cacheEntry.str, l.str) &&
           cacheEntry.callerScript == l.callerScript &&
           script->getVersion() == l.version &&
           cacheEntry.pc == l.pc;
}

// Add the script to the eval cache when EvalKernel is finished
class EvalScriptGuard
{
    JSContext* cx_;
    Rooted<JSScript*> script_;

    /* These fields are only valid if lookup_.str is non-nullptr. */
    EvalCacheLookup lookup_;
    EvalCache::AddPtr p_;

    RootedLinearString lookupStr_;

  public:
    explicit EvalScriptGuard(JSContext* cx)
        : cx_(cx), script_(cx), lookup_(cx), lookupStr_(cx) {}

    ~EvalScriptGuard() {
        if (script_) {
            script_->cacheForEval();
            EvalCacheEntry cacheEntry = {lookupStr_, script_, lookup_.callerScript, lookup_.pc};
            lookup_.str = lookupStr_;
            if (lookup_.str && IsEvalCacheCandidate(script_))
                cx_->runtime()->evalCache.relookupOrAdd(p_, lookup_, cacheEntry);
        }
    }

    void lookupInEvalCache(JSLinearString* str, JSScript* callerScript, jsbytecode* pc)
    {
        lookupStr_ = str;
        lookup_.str = str;
        lookup_.callerScript = callerScript;
        lookup_.version = cx_->findVersion();
        lookup_.pc = pc;
        p_ = cx_->runtime()->evalCache.lookupForAdd(lookup_);
        if (p_) {
            script_ = p_->script;
            cx_->runtime()->evalCache.remove(p_);
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
            for (RangedPtr<const CharT> cp = chars.start() + 1, end = chars.end() - 1;
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
                     : mozilla::Range<const CharT>(chars.start().get() + 1U, len - 2);

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

// Define subset of ExecuteType so that casting performs the injection.
enum EvalType { DIRECT_EVAL = EXECUTE_DIRECT_EVAL, INDIRECT_EVAL = EXECUTE_INDIRECT_EVAL };

// Common code implementing direct and indirect eval.
//
// Evaluate call.argv[2], if it is a string, in the context of the given calling
// frame, with the provided scope chain, with the semantics of either a direct
// or indirect eval (see ES5 10.4.2).  If this is an indirect eval, scopeobj
// must be a global object.
//
// On success, store the completion value in call.rval and return true.
static bool
EvalKernel(JSContext* cx, const CallArgs& args, EvalType evalType, AbstractFramePtr caller,
           HandleObject scopeobj, jsbytecode* pc)
{
    MOZ_ASSERT((evalType == INDIRECT_EVAL) == !caller);
    MOZ_ASSERT((evalType == INDIRECT_EVAL) == !pc);
    MOZ_ASSERT_IF(evalType == INDIRECT_EVAL, IsGlobalLexicalScope(scopeobj));
    AssertInnerizedScopeChain(cx, *scopeobj);

    Rooted<GlobalObject*> scopeObjGlobal(cx, &scopeobj->global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, scopeObjGlobal)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_EVAL);
        return false;
    }

    // ES5 15.1.2.1 step 1.
    if (args.length() < 1) {
        args.rval().setUndefined();
        return true;
    }
    if (!args[0].isString()) {
        args.rval().set(args[0]);
        return true;
    }
    RootedString str(cx, args[0].toString());

    // ES5 15.1.2.1 steps 2-8.

    // Per ES5, indirect eval runs in the global scope. (eval is specified this
    // way so that the compiler can make assumptions about what bindings may or
    // may not exist in the current frame if it doesn't see 'eval'.)
    MOZ_ASSERT_IF(evalType != DIRECT_EVAL,
                  args.callee().global() == scopeobj->as<ClonedBlockObject>().global());

    RootedLinearString linearStr(cx, str->ensureLinear(cx));
    if (!linearStr)
        return false;

    RootedScript callerScript(cx, caller ? caller.script() : nullptr);
    EvalJSONResult ejr = TryEvalJSON(cx, linearStr, args.rval());
    if (ejr != EvalJSON_NotJSON)
        return ejr == EvalJSON_Success;

    EvalScriptGuard esg(cx);

    if (evalType == DIRECT_EVAL && caller.isNonEvalFunctionFrame())
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

        RootedObject enclosing(cx);
        if (evalType == DIRECT_EVAL)
            enclosing = callerScript->innermostStaticScope(pc);
        else
            enclosing = &cx->global()->lexicalScope().staticBlock();
        Rooted<StaticEvalObject*> staticScope(cx, StaticEvalObject::create(cx, enclosing));
        if (!staticScope)
            return false;

        CompileOptions options(cx);
        options.setFileAndLine(filename, 1)
               .setIsRunOnce(true)
               .setForEval(true)
               .setNoScriptRval(false)
               .setMutedErrors(mutedErrors)
               .setIntroductionInfo(introducerFilename, "eval", lineno, maybeScript, pcOffset)
               .maybeMakeStrictMode(evalType == DIRECT_EVAL && IsStrictEvalPC(pc));

        AutoStableStringChars linearChars(cx);
        if (!linearChars.initTwoByte(cx, linearStr))
            return false;

        const char16_t* chars = linearChars.twoByteRange().start().get();
        SourceBufferHolder::Ownership ownership = linearChars.maybeGiveOwnershipToCaller()
                                                  ? SourceBufferHolder::GiveOwnership
                                                  : SourceBufferHolder::NoOwnership;
        SourceBufferHolder srcBuf(chars, linearStr->length(), ownership);
        JSScript* compiled = frontend::CompileScript(cx, &cx->tempLifoAlloc(),
                                                     scopeobj, staticScope, callerScript,
                                                     options, srcBuf, linearStr);
        if (!compiled)
            return false;

        if (compiled->strict())
            staticScope->setStrict();

        esg.setNewScript(compiled);
    }

    // Look up the newTarget from the frame iterator.
    Value newTargetVal = NullValue();
    return ExecuteKernel(cx, esg.script(), *scopeobj, newTargetVal, ExecuteType(evalType),
                         NullFramePtr() /* evalInFrame */, args.rval().address());
}

bool
js::DirectEvalStringFromIon(JSContext* cx,
                            HandleObject scopeobj, HandleScript callerScript,
                            HandleValue newTargetValue, HandleString str,
                            jsbytecode* pc, MutableHandleValue vp)
{
    AssertInnerizedScopeChain(cx, *scopeobj);

    Rooted<GlobalObject*> scopeObjGlobal(cx, &scopeobj->global());
    if (!GlobalObject::isRuntimeCodeGenEnabled(cx, scopeObjGlobal)) {
        JS_ReportErrorNumber(cx, GetErrorMessage, nullptr, JSMSG_CSP_BLOCKED_EVAL);
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

        RootedObject enclosing(cx, callerScript->innermostStaticScope(pc));
        Rooted<StaticEvalObject*> staticScope(cx, StaticEvalObject::create(cx, enclosing));
        if (!staticScope)
            return false;

        CompileOptions options(cx);
        options.setFileAndLine(filename, 1)
               .setIsRunOnce(true)
               .setForEval(true)
               .setNoScriptRval(false)
               .setMutedErrors(mutedErrors)
               .setIntroductionInfo(introducerFilename, "eval", lineno, maybeScript, pcOffset)
               .maybeMakeStrictMode(IsStrictEvalPC(pc));

        AutoStableStringChars linearChars(cx);
        if (!linearChars.initTwoByte(cx, linearStr))
            return false;

        const char16_t* chars = linearChars.twoByteRange().start().get();
        SourceBufferHolder::Ownership ownership = linearChars.maybeGiveOwnershipToCaller()
                                                  ? SourceBufferHolder::GiveOwnership
                                                  : SourceBufferHolder::NoOwnership;
        SourceBufferHolder srcBuf(chars, linearStr->length(), ownership);
        JSScript* compiled = frontend::CompileScript(cx, &cx->tempLifoAlloc(),
                                                     scopeobj, staticScope, callerScript,
                                                     options, srcBuf, linearStr);
        if (!compiled)
            return false;

        if (compiled->strict())
            staticScope->setStrict();

        esg.setNewScript(compiled);
    }

    return ExecuteKernel(cx, esg.script(), *scopeobj, newTargetValue,
                         ExecuteType(DIRECT_EVAL), NullFramePtr() /* evalInFrame */, vp.address());
}

bool
js::IndirectEval(JSContext* cx, unsigned argc, Value* vp)
{
    CallArgs args = CallArgsFromVp(argc, vp);
    Rooted<GlobalObject*> global(cx, &args.callee().global());
    RootedObject globalLexical(cx, &global->lexicalScope());
    return EvalKernel(cx, args, INDIRECT_EVAL, NullFramePtr(), globalLexical, nullptr);
}

bool
js::DirectEval(JSContext* cx, const CallArgs& args)
{
    // Direct eval can assume it was called from an interpreted or baseline frame.
    ScriptFrameIter iter(cx);
    AbstractFramePtr caller = iter.abstractFramePtr();

    MOZ_ASSERT(caller.scopeChain()->global().valueIsEval(args.calleev()));
    MOZ_ASSERT(JSOp(*iter.pc()) == JSOP_EVAL ||
               JSOp(*iter.pc()) == JSOP_STRICTEVAL ||
               JSOp(*iter.pc()) == JSOP_SPREADEVAL ||
               JSOp(*iter.pc()) == JSOP_STRICTSPREADEVAL);
    MOZ_ASSERT_IF(caller.isFunctionFrame(),
                  caller.compartment() == caller.callee()->compartment());

    RootedObject scopeChain(cx, caller.scopeChain());
    return EvalKernel(cx, args, DIRECT_EVAL, caller, scopeChain, iter.pc());
}

bool
js::IsAnyBuiltinEval(JSFunction* fun)
{
    return fun->maybeNative() == IndirectEval;
}

JS_FRIEND_API(bool)
js::ExecuteInGlobalAndReturnScope(JSContext* cx, HandleObject global, HandleScript scriptArg,
                                  MutableHandleObject scopeArg)
{
    CHECK_REQUEST(cx);
    assertSameCompartment(cx, global);
    MOZ_ASSERT(global->is<GlobalObject>());
    MOZ_RELEASE_ASSERT(scriptArg->hasNonSyntacticScope());

    RootedScript script(cx, scriptArg);
    Rooted<GlobalObject*> globalRoot(cx, &global->as<GlobalObject>());
    if (script->compartment() != cx->compartment()) {
        Rooted<ScopeObject*> staticScope(cx, &globalRoot->lexicalScope().staticBlock());
        staticScope = StaticNonSyntacticScopeObjects::create(cx, staticScope);
        if (!staticScope)
            return false;
        script = CloneGlobalScript(cx, staticScope, script);
        if (!script)
            return false;

        Debugger::onNewScript(cx, script);
    }

    Rooted<ClonedBlockObject*> globalLexical(cx, &globalRoot->lexicalScope());
    Rooted<ScopeObject*> scope(cx, NonSyntacticVariablesObject::create(cx, globalLexical));
    if (!scope)
        return false;

    // Unlike the non-syntactic scope chain API used by the subscript loader,
    // this API creates a fresh block scope each time.
    RootedObject enclosingStaticScope(cx, script->enclosingStaticScope());
    scope = ClonedBlockObject::createNonSyntactic(cx, enclosingStaticScope, scope);
    if (!scope)
        return false;

    RootedValue rval(cx);
    if (!ExecuteKernel(cx, script, *scope, UndefinedValue(), EXECUTE_GLOBAL,
                       NullFramePtr() /* evalInFrame */, rval.address()))
    {
        return false;
    }

    scopeArg.set(scope);
    return true;
}
