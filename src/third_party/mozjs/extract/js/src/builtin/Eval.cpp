/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "builtin/Eval.h"

#include "mozilla/HashFunctions.h"
#include "mozilla/Range.h"

#include "frontend/BytecodeCompiler.h"  // frontend::CompileEvalScript
#include "gc/HashUtil.h"
#include "js/CompilationAndEvaluation.h"
#include "js/EnvironmentChain.h"       // JS::EnvironmentChain
#include "js/friend/ErrorMessages.h"   // js::GetErrorMessage, JSMSG_*
#include "js/friend/JSMEnvironment.h"  // JS::NewJSMEnvironment, JS::ExecuteInJSMEnvironment, JS::GetJSMEnvironmentOfScriptedCaller, JS::IsJSMEnvironment
#include "js/friend/WindowProxy.h"     // js::IsWindowProxy
#include "js/SourceText.h"
#include "js/StableStringChars.h"
#include "vm/EnvironmentObject.h"
#include "vm/FrameIter.h"
#include "vm/GlobalObject.h"
#include "vm/Interpreter.h"
#include "vm/JSContext.h"
#include "vm/JSONParser.h"

#include "gc/Marking-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using mozilla::AddToHash;

using JS::AutoCheckCannotGC;
using JS::AutoStableStringChars;
using JS::CompileOptions;
using JS::SourceText;

// We should be able to assert this for *any* fp->environmentChain().
static void AssertInnerizedEnvironmentChain(JSContext* cx, JSObject& env) {
#ifdef DEBUG
  RootedObject obj(cx);
  for (obj = &env; obj; obj = obj->enclosingEnvironment()) {
    MOZ_ASSERT(!IsWindowProxy(obj));
  }
#endif
}

static bool IsEvalCacheCandidate(JSScript* script) {
  if (!script->isDirectEvalInFunction()) {
    return false;
  }

  // Make sure there are no inner objects (which may be used directly by script
  // and clobbered) or inner functions (which may have wrong scope).
  for (JS::GCCellPtr gcThing : script->gcthings()) {
    if (gcThing.is<JSObject>()) {
      return false;
    }
  }

  return true;
}

/* static */
HashNumber EvalCacheHashPolicy::hash(const EvalCacheLookup& l) {
  HashNumber hash = HashStringChars(l.str);
  return AddToHash(hash, l.callerScript, l.pc);
}

/* static */
bool EvalCacheHashPolicy::match(const EvalCacheEntry& cacheEntry,
                                const EvalCacheLookup& l) {
  MOZ_ASSERT(IsEvalCacheCandidate(cacheEntry.script));

  return EqualStrings(cacheEntry.str, l.str) &&
         cacheEntry.callerScript == l.callerScript && cacheEntry.pc == l.pc;
}

void EvalCacheLookup::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &str, "EvalCacheLookup::str");
  TraceNullableRoot(trc, &callerScript, "EvalCacheLookup::callerScript");
}

// Add the script to the eval cache when EvalKernel is finished
class EvalScriptGuard {
  JSContext* cx_;
  Rooted<JSScript*> script_;

  /* These fields are only valid if lookup_.str is non-nullptr. */
  Rooted<EvalCacheLookup> lookup_;
  mozilla::Maybe<DependentAddPtr<EvalCache>> p_;

  Rooted<JSLinearString*> lookupStr_;

 public:
  explicit EvalScriptGuard(JSContext* cx)
      : cx_(cx), script_(cx), lookup_(cx), lookupStr_(cx) {}

  ~EvalScriptGuard() {
    if (script_ && !cx_->isExceptionPending()) {
      script_->cacheForEval();
      EvalCacheLookup& lookup = lookup_.get();
      EvalCacheEntry cacheEntry = {lookupStr_, script_, lookup.callerScript,
                                   lookup.pc};
      lookup.str = lookupStr_;
      if (lookup.str && IsEvalCacheCandidate(script_)) {
        // Ignore failure to add cache entry.
        if (!p_->add(cx_, cx_->caches().evalCache, lookup, cacheEntry)) {
          cx_->recoverFromOutOfMemory();
        }
      }
    }
  }

  void lookupInEvalCache(JSLinearString* str, JSScript* callerScript,
                         jsbytecode* pc) {
    lookupStr_ = str;
    EvalCacheLookup& lookup = lookup_.get();
    lookup.str = str;
    lookup.callerScript = callerScript;
    lookup.pc = pc;
    p_.emplace(cx_, cx_->caches().evalCache, lookup);
    if (*p_) {
      script_ = (*p_)->script;
      p_->remove(cx_, cx_->caches().evalCache, lookup);
    }
  }

  void setNewScript(JSScript* script) {
    // JSScript::fullyInitFromStencil has already called js_CallNewScriptHook.
    MOZ_ASSERT(!script_ && script);
    script_ = script;
  }

  bool foundScript() { return !!script_; }

  HandleScript script() {
    MOZ_ASSERT(script_);
    return script_;
  }
};

enum class EvalJSONResult { Failure, Success, NotJSON };

template <typename CharT>
static bool EvalStringMightBeJSON(const mozilla::Range<const CharT> chars) {
  // If the eval string starts with '(' or '[' and ends with ')' or ']', it
  // may be JSON.  Try the JSON parser first because it's much faster.  If
  // the eval string isn't JSON, JSON parsing will probably fail quickly, so
  // little time will be lost.
  size_t length = chars.length();
  if (length < 2) {
    return false;
  }

  // It used to be that strings in JavaScript forbid U+2028 LINE SEPARATOR
  // and U+2029 PARAGRAPH SEPARATOR, so something like
  //
  //   eval("['" + "\u2028" + "']");
  //
  // i.e. an array containing a string with a line separator in it, *would*
  // be JSON but *would not* be valid JavaScript.  Handing such a string to
  // the JSON parser would then fail to recognize a syntax error.  As of
  // <https://tc39.github.io/proposal-json-superset/> JavaScript strings may
  // contain these two code points, so it's safe to JSON-parse eval strings
  // that contain them.

  CharT first = chars[0], last = chars[length - 1];
  return (first == '[' && last == ']') || (first == '(' && last == ')');
}

template <typename CharT>
static EvalJSONResult ParseEvalStringAsJSON(
    JSContext* cx, const mozilla::Range<const CharT> chars,
    MutableHandleValue rval) {
  size_t len = chars.length();
  MOZ_ASSERT((chars[0] == '(' && chars[len - 1] == ')') ||
             (chars[0] == '[' && chars[len - 1] == ']'));

  auto jsonChars = (chars[0] == '[') ? chars
                                     : mozilla::Range<const CharT>(
                                           chars.begin().get() + 1U, len - 2);

  Rooted<JSONParser<CharT>> parser(
      cx, cx, jsonChars, JSONParser<CharT>::ParseType::AttemptForEval);
  if (!parser.parse(rval)) {
    return EvalJSONResult::Failure;
  }

  return rval.isUndefined() ? EvalJSONResult::NotJSON : EvalJSONResult::Success;
}

static EvalJSONResult TryEvalJSON(JSContext* cx, JSLinearString* str,
                                  MutableHandleValue rval) {
  if (str->hasLatin1Chars()) {
    AutoCheckCannotGC nogc;
    if (!EvalStringMightBeJSON(str->latin1Range(nogc))) {
      return EvalJSONResult::NotJSON;
    }
  } else {
    AutoCheckCannotGC nogc;
    if (!EvalStringMightBeJSON(str->twoByteRange(nogc))) {
      return EvalJSONResult::NotJSON;
    }
  }

  AutoStableStringChars linearChars(cx);
  if (!linearChars.init(cx, str)) {
    return EvalJSONResult::Failure;
  }

  return linearChars.isLatin1()
             ? ParseEvalStringAsJSON(cx, linearChars.latin1Range(), rval)
             : ParseEvalStringAsJSON(cx, linearChars.twoByteRange(), rval);
}

enum EvalType { DIRECT_EVAL, INDIRECT_EVAL };

// 18.2.1.1 PerformEval
//
// Common code implementing direct and indirect eval.
//
// Evaluate v, if it is a string, in the context of the given calling
// frame, with the provided scope chain, with the semantics of either a direct
// or indirect eval (see ES5 10.4.2).  If this is an indirect eval, env
// must be the global lexical environment.
//
// On success, store the completion value in call.rval and return true.
static bool EvalKernel(JSContext* cx, HandleValue v, EvalType evalType,
                       AbstractFramePtr caller, HandleObject env,
                       jsbytecode* pc, MutableHandleValue vp) {
  MOZ_ASSERT((evalType == INDIRECT_EVAL) == !caller);
  MOZ_ASSERT((evalType == INDIRECT_EVAL) == !pc);
  MOZ_ASSERT_IF(evalType == INDIRECT_EVAL,
                env->is<GlobalLexicalEnvironmentObject>());
  AssertInnerizedEnvironmentChain(cx, *env);

  // "Dynamic Code Brand Checks" adds support for Object values.
  // https://tc39.es/proposal-dynamic-code-brand-checks/#sec-performeval
  // Steps 2-4.
  RootedString str(cx);
  if (v.isString()) {
    str = v.toString();
  } else if (v.isObject()) {
    RootedObject obj(cx, &v.toObject());
    if (!cx->getCodeForEval(obj, &str)) {
      return false;
    }
  }
  if (!str) {
    vp.set(v);
    return true;
  }

  // Steps 6-8.
  JS::RootedVector<JSString*> parameterStrings(cx);
  JS::RootedVector<Value> parameterArgs(cx);
  bool canCompileStrings = false;
  if (!cx->isRuntimeCodeGenEnabled(
          JS::RuntimeCode::JS, str,
          evalType == DIRECT_EVAL ? JS::CompilationType::DirectEval
                                  : JS::CompilationType::IndirectEval,
          parameterStrings, str, parameterArgs, v, &canCompileStrings)) {
    return false;
  }
  if (!canCompileStrings) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_CSP_BLOCKED_EVAL);
    return false;
  }

  // Step 9 ff.

  // Per ES5, indirect eval runs in the global scope. (eval is specified this
  // way so that the compiler can make assumptions about what bindings may or
  // may not exist in the current frame if it doesn't see 'eval'.)
  MOZ_ASSERT_IF(
      evalType != DIRECT_EVAL,
      cx->global() == &env->as<GlobalLexicalEnvironmentObject>().global());

  Rooted<JSLinearString*> linearStr(cx, str->ensureLinear(cx));
  if (!linearStr) {
    return false;
  }

  RootedScript callerScript(cx, caller ? caller.script() : nullptr);
  EvalJSONResult ejr = TryEvalJSON(cx, linearStr, vp);
  if (ejr != EvalJSONResult::NotJSON) {
    return ejr == EvalJSONResult::Success;
  }

  EvalScriptGuard esg(cx);

  if (evalType == DIRECT_EVAL && caller.isFunctionFrame()) {
    esg.lookupInEvalCache(linearStr, callerScript, pc);
  }

  if (!esg.foundScript()) {
    RootedScript maybeScript(cx);
    uint32_t lineno;
    const char* filename;
    bool mutedErrors;
    uint32_t pcOffset;
    if (evalType == DIRECT_EVAL) {
      DescribeScriptedCallerForDirectEval(cx, callerScript, pc, &filename,
                                          &lineno, &pcOffset, &mutedErrors);
      maybeScript = callerScript;
    } else {
      DescribeScriptedCallerForCompilation(cx, &maybeScript, &filename, &lineno,
                                           &pcOffset, &mutedErrors);
    }

    const char* introducerFilename = filename;
    if (maybeScript && maybeScript->scriptSource()->introducerFilename()) {
      introducerFilename = maybeScript->scriptSource()->introducerFilename();
    }

    Rooted<Scope*> enclosing(cx);
    if (evalType == DIRECT_EVAL) {
      enclosing = callerScript->innermostScope(pc);
    } else {
      enclosing = &cx->global()->emptyGlobalScope();
    }

    CompileOptions options(cx);
    options.setIsRunOnce(true)
        .setNoScriptRval(false)
        .setMutedErrors(mutedErrors)
        .setDeferDebugMetadata();

    RootedScript introScript(cx);

    if (evalType == DIRECT_EVAL && IsStrictEvalPC(pc)) {
      options.setForceStrictMode();
    }

    if (introducerFilename) {
      options.setFileAndLine(filename, 1);
      options.setIntroductionInfo(introducerFilename, "eval", lineno, pcOffset);
      introScript = maybeScript;
    } else {
      options.setFileAndLine("eval", 1);
      options.setIntroductionType("eval");
    }
    options.setNonSyntacticScope(
        enclosing->hasOnChain(ScopeKind::NonSyntactic));

    AutoStableStringChars linearChars(cx);
    if (!linearChars.initTwoByte(cx, linearStr)) {
      return false;
    }

    SourceText<char16_t> srcBuf;
    if (!srcBuf.initMaybeBorrowed(cx, linearChars)) {
      return false;
    }

    RootedScript script(
        cx, frontend::CompileEvalScript(cx, options, srcBuf, enclosing, env));
    if (!script) {
      return false;
    }

    RootedValue undefValue(cx);
    JS::InstantiateOptions instantiateOptions(options);
    if (!JS::UpdateDebugMetadata(cx, script, instantiateOptions, undefValue,
                                 nullptr, introScript, maybeScript)) {
      return false;
    }

    esg.setNewScript(script);
  }

  return ExecuteKernel(cx, esg.script(), env, NullFramePtr() /* evalInFrame */,
                       vp);
}

bool js::IndirectEval(JSContext* cx, unsigned argc, Value* vp) {
  CallArgs args = CallArgsFromVp(argc, vp);

  RootedObject globalLexical(cx, &cx->global()->lexicalEnvironment());

  // Note we'll just pass |undefined| here, then return it directly (or throw
  // if runtime codegen is disabled), if no argument is provided.
  return EvalKernel(cx, args.get(0), INDIRECT_EVAL, NullFramePtr(),
                    globalLexical, nullptr, args.rval());
}

bool js::DirectEval(JSContext* cx, HandleValue v, MutableHandleValue vp) {
  // Direct eval can assume it was called from an interpreted or baseline frame.
  ScriptFrameIter iter(cx);
  AbstractFramePtr caller = iter.abstractFramePtr();

  MOZ_ASSERT(JSOp(*iter.pc()) == JSOp::Eval ||
             JSOp(*iter.pc()) == JSOp::StrictEval ||
             JSOp(*iter.pc()) == JSOp::SpreadEval ||
             JSOp(*iter.pc()) == JSOp::StrictSpreadEval);
  MOZ_ASSERT(caller.realm() == caller.script()->realm());

  RootedObject envChain(cx, caller.environmentChain());
  return EvalKernel(cx, v, DIRECT_EVAL, caller, envChain, iter.pc(), vp);
}

bool js::IsAnyBuiltinEval(JSFunction* fun) {
  return fun->maybeNative() == IndirectEval;
}

static bool ExecuteInExtensibleLexicalEnvironment(
    JSContext* cx, HandleScript scriptArg,
    Handle<ExtensibleLexicalEnvironmentObject*> env) {
  CHECK_THREAD(cx);
  cx->check(env);
  cx->check(scriptArg);
  MOZ_RELEASE_ASSERT(scriptArg->hasNonSyntacticScope());

  RootedValue rval(cx);
  return ExecuteKernel(cx, scriptArg, env, NullFramePtr() /* evalInFrame */,
                       &rval);
}

JS_PUBLIC_API bool js::ExecuteInFrameScriptEnvironment(
    JSContext* cx, HandleObject objArg, HandleScript scriptArg,
    MutableHandleObject envArg) {
  Rooted<NonSyntacticVariablesObject*> varEnv(
      cx, NonSyntacticVariablesObject::create(cx));
  if (!varEnv) {
    return false;
  }

  JS::EnvironmentChain envChain(cx, JS::SupportUnscopables::No);
  if (!envChain.append(objArg)) {
    return false;
  }

  Rooted<WithEnvironmentObject*> env(
      cx, js::CreateObjectsForEnvironmentChain(cx, envChain, varEnv));
  if (!env) {
    return false;
  }

  // Create lexical environment with |this| == objArg, which should be a Gecko
  // MessageManager.
  // NOTE: This is required behavior for Gecko FrameScriptLoader, where some
  // callers try to bind methods from the message manager in their scope chain
  // to |this|, and will fail if it is not bound to a message manager.
  ObjectRealm& realm = ObjectRealm::get(varEnv);
  Rooted<NonSyntacticLexicalEnvironmentObject*> lexicalEnv(
      cx, realm.getOrCreateNonSyntacticLexicalEnvironment(cx, env, varEnv));
  if (!lexicalEnv) {
    return false;
  }

  if (!ExecuteInExtensibleLexicalEnvironment(cx, scriptArg, lexicalEnv)) {
    return false;
  }

  envArg.set(lexicalEnv);
  return true;
}

JS_PUBLIC_API JSObject* JS::NewJSMEnvironment(JSContext* cx) {
  Rooted<NonSyntacticVariablesObject*> varEnv(
      cx, NonSyntacticVariablesObject::create(cx));
  if (!varEnv) {
    return nullptr;
  }

  // Force the NonSyntacticLexicalEnvironmentObject to be created.
  ObjectRealm& realm = ObjectRealm::get(varEnv);
  MOZ_ASSERT(!realm.getNonSyntacticLexicalEnvironment(varEnv));
  if (!realm.getOrCreateNonSyntacticLexicalEnvironment(cx, varEnv)) {
    return nullptr;
  }

  return varEnv;
}

JS_PUBLIC_API bool JS::ExecuteInJSMEnvironment(JSContext* cx,
                                               HandleScript scriptArg,
                                               HandleObject varEnv) {
  JS::EnvironmentChain emptyChain(cx, JS::SupportUnscopables::No);
  return ExecuteInJSMEnvironment(cx, scriptArg, varEnv, emptyChain);
}

JS_PUBLIC_API bool JS::ExecuteInJSMEnvironment(
    JSContext* cx, HandleScript scriptArg, HandleObject varEnv,
    const EnvironmentChain& targetObj) {
  cx->check(varEnv);
  MOZ_ASSERT(
      ObjectRealm::get(varEnv).getNonSyntacticLexicalEnvironment(varEnv));
  MOZ_DIAGNOSTIC_ASSERT(scriptArg->noScriptRval());

  Rooted<ExtensibleLexicalEnvironmentObject*> env(
      cx, ExtensibleLexicalEnvironmentObject::forVarEnvironment(varEnv));

  // If the Gecko subscript loader specifies target objects, we need to add
  // them to the environment. These are added after the NSVO environment.
  if (!targetObj.empty()) {
    // The environment chain will be as follows:
    //      GlobalObject / SystemGlobal
    //      GlobalLexicalEnvironmentObject[this=global]
    //      NonSyntacticVariablesObject (the JSMEnvironment)
    //      NonSyntacticLexicalEnvironmentObject[this=nsvo]
    //      WithEnvironmentObject[target=targetObj]
    //      NonSyntacticLexicalEnvironmentObject[this=targetObj] (*)
    //
    //  (*) This environment intercepts JSOp::GlobalThis.

    // Wrap the target objects in WithEnvironments.
    Rooted<WithEnvironmentObject*> envChain(
        cx, js::CreateObjectsForEnvironmentChain(cx, targetObj, env));
    if (!envChain) {
      return false;
    }

    // See CreateNonSyntacticEnvironmentChain
    if (!JSObject::setQualifiedVarObj(cx, envChain)) {
      return false;
    }

    // Create an extensible lexical environment for the target object.
    env = ObjectRealm::get(envChain).getOrCreateNonSyntacticLexicalEnvironment(
        cx, envChain);
    if (!env) {
      return false;
    }
  }

  return ExecuteInExtensibleLexicalEnvironment(cx, scriptArg, env);
}

JS_PUBLIC_API JSObject* JS::GetJSMEnvironmentOfScriptedCaller(JSContext* cx) {
  FrameIter iter(cx);
  if (iter.done()) {
    return nullptr;
  }

  // WASM frames don't always provide their environment, but we also shouldn't
  // expect to see any calling into here.
  MOZ_RELEASE_ASSERT(!iter.isWasm());

  RootedObject env(cx, iter.environmentChain(cx));
  while (env && !env->is<NonSyntacticVariablesObject>()) {
    env = env->enclosingEnvironment();
  }

  return env;
}

JS_PUBLIC_API bool JS::IsJSMEnvironment(JSObject* obj) {
  // NOTE: This also returns true if the NonSyntacticVariablesObject was
  // created for reasons other than the JSM loader.
  return obj->is<NonSyntacticVariablesObject>();
}

#ifdef JSGC_HASH_TABLE_CHECKS
void RuntimeCaches::checkEvalCacheAfterMinorGC() {
  gc::CheckTableAfterMovingGC(evalCache, [](const auto& entry) {
    CheckGCThingAfterMovingGC(entry.str);
    CheckGCThingAfterMovingGC(entry.script);
    CheckGCThingAfterMovingGC(entry.callerScript);
    return EvalCacheLookup(entry.str, entry.callerScript, entry.pc);
  });
}
#endif
