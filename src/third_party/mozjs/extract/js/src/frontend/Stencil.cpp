/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "frontend/Stencil.h"

#include "mozilla/AlreadyAddRefed.h"        // already_AddRefed
#include "mozilla/OperatorNewExtensions.h"  // mozilla::KnownNotNull
#include "mozilla/PodOperations.h"          // mozilla::PodCopy
#include "mozilla/RefPtr.h"                 // RefPtr
#include "mozilla/ScopeExit.h"              // mozilla::ScopeExit
#include "mozilla/Sprintf.h"                // SprintfLiteral

#include "ds/LifoAlloc.h"               // LifoAlloc
#include "frontend/AbstractScopePtr.h"  // ScopeIndex
#include "frontend/BytecodeCompilation.h"  // CanLazilyParse, CompileGlobalScriptToStencil
#include "frontend/BytecodeCompiler.h"    // ParseModuleToStencil
#include "frontend/BytecodeSection.h"     // EmitScriptThingsVector
#include "frontend/CompilationStencil.h"  // CompilationStencil, CompilationState, ExtensibleCompilationStencil, CompilationGCOutput, CompilationStencilMerger
#include "frontend/NameAnalysisTypes.h"   // EnvironmentCoordinate
#include "frontend/SharedContext.h"
#include "gc/AllocKind.h"               // gc::AllocKind
#include "gc/Rooting.h"                 // RootedAtom
#include "gc/Tracer.h"                  // TraceNullableRoot
#include "js/CallArgs.h"                // JSNative
#include "js/experimental/JSStencil.h"  // JS::Stencil
#include "js/GCAPI.h"                   // JS::AutoCheckCannotGC
#include "js/RootingAPI.h"              // Rooted
#include "js/Transcoding.h"             // JS::TranscodeBuffer
#include "js/Value.h"                   // ObjectValue
#include "js/WasmModule.h"              // JS::WasmModule
#include "vm/BindingKind.h"             // BindingKind
#include "vm/EnvironmentObject.h"
#include "vm/GeneratorAndAsyncKind.h"  // GeneratorKind, FunctionAsyncKind
#include "vm/HelperThreads.h"          // js::StartOffThreadParseScript
#include "vm/HelperThreadState.h"
#include "vm/JSContext.h"  // JSContext
#include "vm/JSFunction.h"  // JSFunction, GetFunctionPrototype, NewFunctionWithProto
#include "vm/JSObject.h"      // JSObject, TenuredObject
#include "vm/JSONPrinter.h"   // js::JSONPrinter
#include "vm/JSScript.h"      // BaseScript, JSScript
#include "vm/Printer.h"       // js::Fprinter
#include "vm/RegExpObject.h"  // js::RegExpObject
#include "vm/Scope.h"  // Scope, *Scope, ScopeKindString, ScopeIter, ScopeKindIsCatch, BindingIter, GetScopeDataTrailingNames
#include "vm/ScopeKind.h"     // ScopeKind
#include "vm/SelfHosting.h"   // SetClonedSelfHostedFunctionName
#include "vm/StencilEnums.h"  // ImmutableScriptFlagsEnum
#include "vm/StringType.h"    // JSAtom, js::CopyChars
#include "vm/Xdr.h"           // XDRMode, XDRResult, XDREncoder
#include "wasm/AsmJS.h"       // InstantiateAsmJS
#include "wasm/WasmModule.h"  // wasm::Module

#include "vm/EnvironmentObject-inl.h"  // JSObject::enclosingEnvironment
#include "vm/JSFunction-inl.h"         // JSFunction::create

using namespace js;
using namespace js::frontend;

bool ScopeContext::init(JSContext* cx, CompilationInput& input,
                        ParserAtomsTable& parserAtoms, InheritThis inheritThis,
                        JSObject* enclosingEnv) {
  Scope* maybeNonDefaultEnclosingScope = input.maybeNonDefaultEnclosingScope();

  // If this eval is in response to Debugger.Frame.eval, we may have an
  // incomplete scope chain. In order to provide a better debugging experience,
  // we inspect the (optional) environment chain to determine it's enclosing
  // FunctionScope if there is one. If there is no such scope, we use the
  // orignal scope provided.
  //
  // NOTE: This is used to compute the ThisBinding kind and to allow access to
  //       private fields and methods, while other contextual information only
  //       uses the actual scope passed to the compile.
  JS::Rooted<Scope*> effectiveScope(
      cx, determineEffectiveScope(maybeNonDefaultEnclosingScope, enclosingEnv));

  if (inheritThis == InheritThis::Yes) {
    computeThisBinding(effectiveScope);
    computeThisEnvironment(maybeNonDefaultEnclosingScope);
  }
  computeInScope(maybeNonDefaultEnclosingScope);

  cacheEnclosingScope(input.enclosingScope);

  if (input.target == CompilationInput::CompilationTarget::Eval) {
    if (!cacheEnclosingScopeBindingForEval(cx, input, parserAtoms)) {
      return false;
    }
    if (!cachePrivateFieldsForEval(cx, input, enclosingEnv, effectiveScope,
                                   parserAtoms)) {
      return false;
    }
  }

  return true;
}

void ScopeContext::computeThisEnvironment(Scope* enclosingScope) {
  uint32_t envCount = 0;
  for (ScopeIter si(enclosingScope); si; si++) {
    if (si.kind() == ScopeKind::Function) {
      JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();

      // Arrow function inherit the "this" environment of the enclosing script,
      // so continue ignore them.
      if (!fun->isArrow()) {
        allowNewTarget = true;

        if (fun->allowSuperProperty()) {
          allowSuperProperty = true;
          enclosingThisEnvironmentHops = envCount;
        }

        if (fun->isClassConstructor()) {
          memberInitializers =
              fun->baseScript()->useMemberInitializers()
                  ? mozilla::Some(fun->baseScript()->getMemberInitializers())
                  : mozilla::Some(MemberInitializers::Empty());
          MOZ_ASSERT(memberInitializers->valid);
        } else {
          if (fun->isSyntheticFunction()) {
            allowArguments = false;
          }
        }

        if (fun->isDerivedClassConstructor()) {
          allowSuperCall = true;
        }

        // Found the effective "this" environment, so stop.
        return;
      }
    }

    if (si.scope()->hasEnvironment()) {
      envCount++;
    }
  }
}

void ScopeContext::computeThisBinding(Scope* scope) {
  // Inspect the scope-chain.
  for (ScopeIter si(scope); si; si++) {
    if (si.kind() == ScopeKind::Module) {
      thisBinding = ThisBinding::Module;
      return;
    }

    if (si.kind() == ScopeKind::Function) {
      JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();

      // Arrow functions don't have their own `this` binding.
      if (fun->isArrow()) {
        continue;
      }

      // Derived class constructors (and their nested arrow functions and evals)
      // use ThisBinding::DerivedConstructor, which ensures TDZ checks happen
      // when accessing |this|.
      if (fun->isDerivedClassConstructor()) {
        thisBinding = ThisBinding::DerivedConstructor;
      } else {
        thisBinding = ThisBinding::Function;
      }

      return;
    }
  }

  thisBinding = ThisBinding::Global;
}

void ScopeContext::computeInScope(Scope* enclosingScope) {
  for (ScopeIter si(enclosingScope); si; si++) {
    if (si.kind() == ScopeKind::ClassBody) {
      inClass = true;
    }

    if (si.kind() == ScopeKind::With) {
      inWith = true;
    }
  }
}

void ScopeContext::cacheEnclosingScope(Scope* enclosingScope) {
  if (!enclosingScope) {
    return;
  }

  enclosingScopeEnvironmentChainLength =
      enclosingScope->environmentChainLength();

  enclosingScopeKind = enclosingScope->kind();

  if (enclosingScope->is<FunctionScope>()) {
    MOZ_ASSERT(enclosingScope->as<FunctionScope>().canonicalFunction());
    enclosingScopeIsArrow =
        enclosingScope->as<FunctionScope>().canonicalFunction()->isArrow();
  }

  enclosingScopeHasEnvironment = enclosingScope->hasEnvironment();

#ifdef DEBUG
  hasNonSyntacticScopeOnChain =
      enclosingScope->hasOnChain(ScopeKind::NonSyntactic);

  // This computes a general answer for the query "does the enclosing scope
  // have a function scope that needs a home object?", but it's only asserted
  // if the parser parses eval body that contains `super` that needs a home
  // object.
  for (ScopeIter si(enclosingScope); si; si++) {
    if (si.kind() == ScopeKind::Function) {
      JSFunction* fun = si.scope()->as<FunctionScope>().canonicalFunction();
      if (fun->isArrow()) {
        continue;
      }
      if (fun->allowSuperProperty() && fun->baseScript()->needsHomeObject()) {
        hasFunctionNeedsHomeObjectOnChain = true;
      }
      break;
    }
  }
#endif
}

Scope* ScopeContext::determineEffectiveScope(Scope* scope,
                                             JSObject* environment) {
  MOZ_ASSERT(effectiveScopeHops == 0);
  // If the scope-chain is non-syntactic, we may still determine a more precise
  // effective-scope to use instead.
  if (environment && scope->hasOnChain(ScopeKind::NonSyntactic)) {
    JSObject* env = environment;
    while (env) {
      // Look at target of any DebugEnvironmentProxy, but be sure to use
      // enclosingEnvironment() of the proxy itself.
      JSObject* unwrapped = env;
      if (env->is<DebugEnvironmentProxy>()) {
        unwrapped = &env->as<DebugEnvironmentProxy>().environment();
#ifdef DEBUG
        enclosingEnvironmentIsDebugProxy_ = true;
#endif
      }

      if (unwrapped->is<CallObject>()) {
        JSFunction* callee = &unwrapped->as<CallObject>().callee();
        return callee->nonLazyScript()->bodyScope();
      }

      env = env->enclosingEnvironment();
      effectiveScopeHops++;
    }
  }

  return scope;
}

bool ScopeContext::cacheEnclosingScopeBindingForEval(
    JSContext* cx, CompilationInput& input, ParserAtomsTable& parserAtoms) {
  enclosingLexicalBindingCache_.emplace();

  js::Scope* varScope =
      EvalScope::nearestVarScopeForDirectEval(input.enclosingScope);
  MOZ_ASSERT(varScope);
  for (ScopeIter si(input.enclosingScope); si; si++) {
    for (js::BindingIter bi(si.scope()); bi; bi++) {
      switch (bi.kind()) {
        case BindingKind::Let: {
          // Annex B.3.5 allows redeclaring simple (non-destructured)
          // catch parameters with var declarations.
          bool annexB35Allowance = si.kind() == ScopeKind::SimpleCatch;
          if (!annexB35Allowance) {
            auto kind = ScopeKindIsCatch(si.kind())
                            ? EnclosingLexicalBindingKind::CatchParameter
                            : EnclosingLexicalBindingKind::Let;
            if (!addToEnclosingLexicalBindingCache(cx, input, parserAtoms,
                                                   bi.name(), kind)) {
              return false;
            }
          }
          break;
        }

        case BindingKind::Const:
          if (!addToEnclosingLexicalBindingCache(
                  cx, input, parserAtoms, bi.name(),
                  EnclosingLexicalBindingKind::Const)) {
            return false;
          }
          break;

        case BindingKind::Synthetic:
          if (!addToEnclosingLexicalBindingCache(
                  cx, input, parserAtoms, bi.name(),
                  EnclosingLexicalBindingKind::Synthetic)) {
            return false;
          }
          break;

        case BindingKind::PrivateMethod:
          if (!addToEnclosingLexicalBindingCache(
                  cx, input, parserAtoms, bi.name(),
                  EnclosingLexicalBindingKind::PrivateMethod)) {
            return false;
          }
          break;

        case BindingKind::Import:
        case BindingKind::FormalParameter:
        case BindingKind::Var:
        case BindingKind::NamedLambdaCallee:
          break;
      }
    }

    if (si.scope() == varScope) {
      break;
    }
  }

  return true;
}

bool ScopeContext::addToEnclosingLexicalBindingCache(
    JSContext* cx, CompilationInput& input, ParserAtomsTable& parserAtoms,
    JSAtom* name, EnclosingLexicalBindingKind kind) {
  auto parserName = parserAtoms.internJSAtom(cx, input.atomCache, name);
  if (!parserName) {
    return false;
  }

  // Same lexical binding can appear multiple times across scopes.
  //
  // enclosingLexicalBindingCache_ map is used for detecting conflicting
  // `var` binding, and inner binding should be reported in the error.
  //
  // cacheEnclosingScopeBindingForEval iterates from inner scope, and
  // inner-most binding is added to the map first.
  //
  // Do not overwrite the value with outer bindings.
  auto p = enclosingLexicalBindingCache_->lookupForAdd(parserName);
  if (!p) {
    if (!enclosingLexicalBindingCache_->add(p, parserName, kind)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

static bool IsPrivateField(JSAtom* atom) {
  MOZ_ASSERT(atom->length() > 0);

  JS::AutoCheckCannotGC nogc;
  if (atom->hasLatin1Chars()) {
    return atom->latin1Chars(nogc)[0] == '#';
  }

  return atom->twoByteChars(nogc)[0] == '#';
}

bool ScopeContext::cachePrivateFieldsForEval(JSContext* cx,
                                             CompilationInput& input,
                                             JSObject* enclosingEnvironment,
                                             Scope* effectiveScope,
                                             ParserAtomsTable& parserAtoms) {
  if (!input.options.privateClassFields) {
    return true;
  }

  effectiveScopePrivateFieldCache_.emplace();

  // We compute an environment coordinate relative to the effective scope
  // environment. In order to safely consume these environment coordinates,
  // we re-map them to include the hops to get the to the effective scope:
  // see EmitterScope::lookupPrivate
  uint32_t hops = effectiveScopeHops;
  for (ScopeIter si(effectiveScope); si; si++) {
    if (si.scope()->kind() == ScopeKind::ClassBody) {
      uint32_t slots = 0;
      for (js::BindingIter bi(si.scope()); bi; bi++) {
        if (bi.kind() == BindingKind::PrivateMethod ||
            (bi.kind() == BindingKind::Synthetic &&
             IsPrivateField(bi.name()))) {
          auto parserName =
              parserAtoms.internJSAtom(cx, input.atomCache, bi.name());
          if (!parserName) {
            return false;
          }

          NameLocation loc =
              NameLocation::DebugEnvironmentCoordinate(bi.kind(), hops, slots);

          if (!effectiveScopePrivateFieldCache_->put(parserName, loc)) {
            ReportOutOfMemory(cx);
            return false;
          }
        }
        slots++;
      }
    }

    // Hops is only consumed by GetAliasedDebugVar, which uses this to
    // traverse the debug environment chain. See the [SMDOC] for Debug
    // Environment Chain, which explains why we don't check for
    // isEnvironment when computing hops here (basically, debug proxies
    // pretend all scopes have environments, even if they were actually
    // optimized out).
    hops++;
  }

  return true;
}

#ifdef DEBUG
static bool NameIsOnEnvironment(Scope* scope, JSAtom* name) {
  for (BindingIter bi(scope); bi; bi++) {
    // If found, the name must already be on the environment or an import,
    // or else there is a bug in the closed-over name analysis in the
    // Parser.
    if (bi.name() == name) {
      BindingLocation::Kind kind = bi.location().kind();

      if (bi.hasArgumentSlot()) {
        JSScript* script = scope->as<FunctionScope>().script();
        if (script->functionAllowsParameterRedeclaration()) {
          // Check for duplicate positional formal parameters.
          for (BindingIter bi2(bi); bi2 && bi2.hasArgumentSlot(); bi2++) {
            if (bi2.name() == name) {
              kind = bi2.location().kind();
            }
          }
        }
      }

      return kind == BindingLocation::Kind::Global ||
             kind == BindingLocation::Kind::Environment ||
             kind == BindingLocation::Kind::Import;
    }
  }

  // If not found, assume it's on the global or dynamically accessed.
  return true;
}
#endif

/* static */
NameLocation ScopeContext::searchInEnclosingScope(JSContext* cx,
                                                  CompilationInput& input,
                                                  ParserAtomsTable& parserAtoms,
                                                  TaggedParserAtomIndex name,
                                                  uint8_t hops) {
  MOZ_ASSERT(input.target ==
                 CompilationInput::CompilationTarget::Delazification ||
             input.target == CompilationInput::CompilationTarget::Eval);

  // TODO-Stencil
  //   Here, we convert our name into a JSAtom*, and hard-crash on failure
  //   to allocate.  This conversion should not be required as we should be
  //   able to iterate up snapshotted scope chains that use parser atoms.
  //
  //   This will be fixed when the enclosing scopes are snapshotted.
  //
  //   See bug 1690277.
  JSAtom* jsname = nullptr;
  {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    jsname = parserAtoms.toJSAtom(cx, name, input.atomCache);
    if (!jsname) {
      oomUnsafe.crash("EmitterScope::searchAndCache");
    }
  }

  for (ScopeIter si(input.enclosingScope); si; si++) {
    MOZ_ASSERT(NameIsOnEnvironment(si.scope(), jsname));

    bool hasEnv = si.hasSyntacticEnvironment();

    switch (si.kind()) {
      case ScopeKind::Function:
        if (hasEnv) {
          JSScript* script = si.scope()->as<FunctionScope>().script();
          if (script->funHasExtensibleScope()) {
            return NameLocation::Dynamic();
          }

          for (BindingIter bi(si.scope()); bi; bi++) {
            if (bi.name() != jsname) {
              continue;
            }

            BindingLocation bindLoc = bi.location();
            if (bi.hasArgumentSlot() &&
                script->functionAllowsParameterRedeclaration()) {
              // Check for duplicate positional formal parameters.
              for (BindingIter bi2(bi); bi2 && bi2.hasArgumentSlot(); bi2++) {
                if (bi2.name() == jsname) {
                  bindLoc = bi2.location();
                }
              }
            }

            MOZ_ASSERT(bindLoc.kind() == BindingLocation::Kind::Environment);
            return NameLocation::EnvironmentCoordinate(bi.kind(), hops,
                                                       bindLoc.slot());
          }
        }
        break;

      case ScopeKind::StrictEval:
      case ScopeKind::FunctionBodyVar:
      case ScopeKind::Lexical:
      case ScopeKind::NamedLambda:
      case ScopeKind::StrictNamedLambda:
      case ScopeKind::SimpleCatch:
      case ScopeKind::Catch:
      case ScopeKind::FunctionLexical:
      case ScopeKind::ClassBody:
        if (hasEnv) {
          for (BindingIter bi(si.scope()); bi; bi++) {
            if (bi.name() != jsname) {
              continue;
            }

            // The name must already have been marked as closed
            // over. If this assertion is hit, there is a bug in the
            // name analysis.
            BindingLocation bindLoc = bi.location();
            MOZ_ASSERT(bindLoc.kind() == BindingLocation::Kind::Environment);
            return NameLocation::EnvironmentCoordinate(bi.kind(), hops,
                                                       bindLoc.slot());
          }
        }
        break;

      case ScopeKind::Module:
        // This case is used only when delazifying a function inside
        // module.
        // Initial compilation of module doesn't have enlcosing scope.
        if (hasEnv) {
          for (BindingIter bi(si.scope()); bi; bi++) {
            if (bi.name() != jsname) {
              continue;
            }

            BindingLocation bindLoc = bi.location();

            // Imports are on the environment but are indirect
            // bindings and must be accessed dynamically instead of
            // using an EnvironmentCoordinate.
            if (bindLoc.kind() == BindingLocation::Kind::Import) {
              MOZ_ASSERT(si.kind() == ScopeKind::Module);
              return NameLocation::Import();
            }

            MOZ_ASSERT(bindLoc.kind() == BindingLocation::Kind::Environment);
            return NameLocation::EnvironmentCoordinate(bi.kind(), hops,
                                                       bindLoc.slot());
          }
        }
        break;

      case ScopeKind::Eval:
        // As an optimization, if the eval doesn't have its own var
        // environment and its immediate enclosing scope is a global
        // scope, all accesses are global.
        if (!hasEnv && si.scope()->enclosing()->is<GlobalScope>()) {
          return NameLocation::Global(BindingKind::Var);
        }
        return NameLocation::Dynamic();

      case ScopeKind::Global:
        return NameLocation::Global(BindingKind::Var);

      case ScopeKind::With:
      case ScopeKind::NonSyntactic:
        return NameLocation::Dynamic();

      case ScopeKind::WasmInstance:
      case ScopeKind::WasmFunction:
        MOZ_CRASH("No direct eval inside wasm functions");
    }

    if (hasEnv) {
      MOZ_ASSERT(hops < ENVCOORD_HOPS_LIMIT - 1);
      hops++;
    }
  }

  MOZ_CRASH("Malformed scope chain");
}

mozilla::Maybe<ScopeContext::EnclosingLexicalBindingKind>
ScopeContext::lookupLexicalBindingInEnclosingScope(TaggedParserAtomIndex name) {
  auto p = enclosingLexicalBindingCache_->lookup(name);
  if (!p) {
    return mozilla::Nothing();
  }

  return mozilla::Some(p->value());
}

bool ScopeContext::effectiveScopePrivateFieldCacheHas(
    TaggedParserAtomIndex name) {
  return effectiveScopePrivateFieldCache_->has(name);
}

mozilla::Maybe<NameLocation> ScopeContext::getPrivateFieldLocation(
    TaggedParserAtomIndex name) {
  // The locations returned by this method are only valid for
  // traversing debug environments.
  //
  // See the comment in cachePrivateFieldsForEval
  MOZ_ASSERT(enclosingEnvironmentIsDebugProxy_);
  auto p = effectiveScopePrivateFieldCache_->lookup(name);
  if (!p) {
    return mozilla::Nothing();
  }
  return mozilla::Some(p->value());
}

bool CompilationInput::initScriptSource(JSContext* cx) {
  source = do_AddRef(cx->new_<ScriptSource>());
  if (!source) {
    return false;
  }

  return source->initFromOptions(cx, options);
}

bool CompilationInput::initForStandaloneFunctionInNonSyntacticScope(
    JSContext* cx, HandleScope functionEnclosingScope) {
  MOZ_ASSERT(!functionEnclosingScope->as<GlobalScope>().isSyntactic());

  target = CompilationTarget::StandaloneFunctionInNonSyntacticScope;
  if (!initScriptSource(cx)) {
    return false;
  }
  enclosingScope = functionEnclosingScope;
  return true;
}

void CompilationInput::trace(JSTracer* trc) {
  atomCache.trace(trc);
  TraceNullableRoot(trc, &lazy_, "compilation-input-lazy");
  TraceNullableRoot(trc, &enclosingScope, "compilation-input-enclosing-scope");
}

void CompilationAtomCache::trace(JSTracer* trc) { atoms_.trace(trc); }

void CompilationGCOutput::trace(JSTracer* trc) {
  TraceNullableRoot(trc, &script, "compilation-gc-output-script");
  TraceNullableRoot(trc, &module, "compilation-gc-output-module");
  TraceNullableRoot(trc, &sourceObject, "compilation-gc-output-source");
  functions.trace(trc);
  scopes.trace(trc);
}

RegExpObject* RegExpStencil::createRegExp(
    JSContext* cx, const CompilationAtomCache& atomCache) const {
  RootedAtom atom(cx, atomCache.getExistingAtomAt(cx, atom_));
  return RegExpObject::createSyntaxChecked(cx, atom, flags(), TenuredObject);
}

RegExpObject* RegExpStencil::createRegExpAndEnsureAtom(
    JSContext* cx, ParserAtomsTable& parserAtoms,
    CompilationAtomCache& atomCache) const {
  RootedAtom atom(cx, parserAtoms.toJSAtom(cx, atom_, atomCache));
  if (!atom) {
    return nullptr;
  }
  return RegExpObject::createSyntaxChecked(cx, atom, flags(), TenuredObject);
}

AbstractScopePtr ScopeStencil::enclosing(
    CompilationState& compilationState) const {
  if (hasEnclosing()) {
    return AbstractScopePtr(compilationState, enclosing());
  }

  return AbstractScopePtr::compilationEnclosingScope(compilationState);
}

Scope* ScopeStencil::enclosingExistingScope(
    const CompilationInput& input, const CompilationGCOutput& gcOutput) const {
  if (hasEnclosing()) {
    Scope* result = gcOutput.scopes[enclosing()];
    MOZ_ASSERT(result, "Scope must already exist to use this method");
    return result;
  }

  return input.enclosingScope;
}

Scope* ScopeStencil::createScope(JSContext* cx, CompilationInput& input,
                                 CompilationGCOutput& gcOutput,
                                 BaseParserScopeData* baseScopeData) const {
  RootedScope enclosingScope(cx, enclosingExistingScope(input, gcOutput));
  return createScope(cx, input.atomCache, enclosingScope, baseScopeData);
}

Scope* ScopeStencil::createScope(JSContext* cx, CompilationAtomCache& atomCache,
                                 HandleScope enclosingScope,
                                 BaseParserScopeData* baseScopeData) const {
  switch (kind()) {
    case ScopeKind::Function: {
      using ScopeType = FunctionScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, CallObject>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical: {
      using ScopeType = LexicalScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, BlockLexicalEnvironmentObject>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::ClassBody: {
      using ScopeType = ClassBodyScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, BlockLexicalEnvironmentObject>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::FunctionBodyVar: {
      using ScopeType = VarScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, VarEnvironmentObject>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic: {
      using ScopeType = GlobalScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, std::nullptr_t>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::Eval:
    case ScopeKind::StrictEval: {
      using ScopeType = EvalScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, VarEnvironmentObject>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::Module: {
      using ScopeType = ModuleScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, ModuleEnvironmentObject>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::With: {
      using ScopeType = WithScope;
      MOZ_ASSERT(matchScopeKind<ScopeType>(kind()));
      return createSpecificScope<ScopeType, std::nullptr_t>(
          cx, atomCache, enclosingScope, baseScopeData);
    }
    case ScopeKind::WasmFunction:
    case ScopeKind::WasmInstance: {
      // ScopeStencil does not support WASM
      break;
    }
  }
  MOZ_CRASH();
}

bool CompilationState::prepareSharedDataStorage(JSContext* cx) {
  size_t allScriptCount = scriptData.length();
  size_t nonLazyScriptCount = nonLazyFunctionCount;
  if (!scriptData[0].isFunction()) {
    nonLazyScriptCount++;
  }
  return sharedData.prepareStorageFor(cx, nonLazyScriptCount, allScriptCount);
}

static bool CreateLazyScript(JSContext* cx, const CompilationInput& input,
                             const CompilationStencil& stencil,
                             CompilationGCOutput& gcOutput,
                             const ScriptStencil& script,
                             const ScriptStencilExtra& scriptExtra,
                             ScriptIndex scriptIndex, HandleFunction function) {
  Rooted<ScriptSourceObject*> sourceObject(cx, gcOutput.sourceObject);

  size_t ngcthings = script.gcThingsLength;

  Rooted<BaseScript*> lazy(
      cx, BaseScript::CreateRawLazy(cx, ngcthings, function, sourceObject,
                                    scriptExtra.extent,
                                    scriptExtra.immutableFlags));
  if (!lazy) {
    return false;
  }

  if (ngcthings) {
    if (!EmitScriptThingsVector(cx, input, stencil, gcOutput,
                                script.gcthings(stencil),
                                lazy->gcthingsForInit())) {
      return false;
    }
  }

  if (scriptExtra.useMemberInitializers()) {
    lazy->setMemberInitializers(scriptExtra.memberInitializers());
  }

  function->initScript(lazy);

  return true;
}

// Parser-generated functions with the same prototype will share the same shape
// and group. By computing the correct values up front, we can save a lot of
// time in the Object creation code. For simplicity, we focus only on plain
// synchronous functions which are by far the most common.
//
// This bypasses the `NewObjectCache`, but callers are expected to retrieve a
// valid group and shape from the appropriate de-duplication tables.
//
// NOTE: Keep this in sync with `js::NewFunctionWithProto`.
static JSFunction* CreateFunctionFast(JSContext* cx, CompilationInput& input,
                                      HandleShape shape,
                                      const ScriptStencil& script,
                                      const ScriptStencilExtra& scriptExtra) {
  MOZ_ASSERT(
      !scriptExtra.immutableFlags.hasFlag(ImmutableScriptFlagsEnum::IsAsync));
  MOZ_ASSERT(!scriptExtra.immutableFlags.hasFlag(
      ImmutableScriptFlagsEnum::IsGenerator));
  MOZ_ASSERT(!script.functionFlags.isAsmJSNative());

  FunctionFlags flags = script.functionFlags;
  gc::AllocKind allocKind = flags.isExtended()
                                ? gc::AllocKind::FUNCTION_EXTENDED
                                : gc::AllocKind::FUNCTION;

  JSFunction* fun;
  JS_TRY_VAR_OR_RETURN_NULL(
      cx, fun, JSFunction::create(cx, allocKind, gc::TenuredHeap, shape));

  fun->setArgCount(scriptExtra.nargs);
  fun->setFlags(flags);

  fun->initScript(nullptr);
  fun->initEnvironment(nullptr);

  if (script.functionAtom) {
    JSAtom* atom = input.atomCache.getExistingAtomAt(cx, script.functionAtom);
    MOZ_ASSERT(atom);
    fun->initAtom(atom);
  }

  if (flags.isExtended()) {
    fun->initializeExtended();
  }

  return fun;
}

static JSFunction* CreateFunction(JSContext* cx, CompilationInput& input,
                                  const CompilationStencil& stencil,
                                  const ScriptStencil& script,
                                  const ScriptStencilExtra& scriptExtra,
                                  ScriptIndex functionIndex) {
  GeneratorKind generatorKind =
      scriptExtra.immutableFlags.hasFlag(ImmutableScriptFlagsEnum::IsGenerator)
          ? GeneratorKind::Generator
          : GeneratorKind::NotGenerator;
  FunctionAsyncKind asyncKind =
      scriptExtra.immutableFlags.hasFlag(ImmutableScriptFlagsEnum::IsAsync)
          ? FunctionAsyncKind::AsyncFunction
          : FunctionAsyncKind::SyncFunction;

  // Determine the new function's proto. This must be done for singleton
  // functions.
  RootedObject proto(cx);
  if (!GetFunctionPrototype(cx, generatorKind, asyncKind, &proto)) {
    return nullptr;
  }

  gc::AllocKind allocKind = script.functionFlags.isExtended()
                                ? gc::AllocKind::FUNCTION_EXTENDED
                                : gc::AllocKind::FUNCTION;
  bool isAsmJS = script.functionFlags.isAsmJSNative();

  JSNative maybeNative = isAsmJS ? InstantiateAsmJS : nullptr;

  RootedAtom displayAtom(cx);
  if (script.functionAtom) {
    displayAtom.set(input.atomCache.getExistingAtomAt(cx, script.functionAtom));
    MOZ_ASSERT(displayAtom);
  }
  RootedFunction fun(
      cx, NewFunctionWithProto(cx, maybeNative, scriptExtra.nargs,
                               script.functionFlags, nullptr, displayAtom,
                               proto, allocKind, TenuredObject));
  if (!fun) {
    return nullptr;
  }

  if (isAsmJS) {
    RefPtr<const JS::WasmModule> asmJS =
        stencil.asmJS->moduleMap.lookup(functionIndex)->value();

    JSObject* moduleObj = asmJS->createObjectForAsmJS(cx);
    if (!moduleObj) {
      return nullptr;
    }

    fun->setExtendedSlot(FunctionExtended::ASMJS_MODULE_SLOT,
                         ObjectValue(*moduleObj));
  }

  return fun;
}

static bool InstantiateAtoms(JSContext* cx, CompilationInput& input,
                             const CompilationStencil& stencil) {
  return InstantiateMarkedAtoms(cx, stencil.parserAtomData, input.atomCache);
}

static bool InstantiateScriptSourceObject(JSContext* cx,
                                          CompilationInput& input,
                                          const CompilationStencil& stencil,
                                          CompilationGCOutput& gcOutput) {
  MOZ_ASSERT(stencil.source);

  gcOutput.sourceObject = ScriptSourceObject::create(cx, stencil.source.get());
  if (!gcOutput.sourceObject) {
    return false;
  }

  // Off-thread compilations do all their GC heap allocation, including the
  // SSO, in a temporary compartment. Hence, for the SSO to refer to the
  // gc-heap-allocated values in |options|, it would need cross-compartment
  // wrappers from the temporary compartment to the real compartment --- which
  // would then be inappropriate once we merged the temporary and real
  // compartments.
  //
  // Instead, we put off populating those SSO slots in off-thread compilations
  // until after we've merged compartments.
  if (!cx->isHelperThreadContext()) {
    Rooted<ScriptSourceObject*> sourceObject(cx, gcOutput.sourceObject);
    if (!ScriptSourceObject::initFromOptions(cx, sourceObject, input.options)) {
      return false;
    }
  }

  return true;
}

// Instantiate ModuleObject. Further initialization is done after the associated
// BaseScript is instantiated in InstantiateTopLevel.
static bool InstantiateModuleObject(JSContext* cx, CompilationInput& input,
                                    const CompilationStencil& stencil,
                                    CompilationGCOutput& gcOutput) {
  MOZ_ASSERT(stencil.scriptExtra[CompilationStencil::TopLevelIndex].isModule());

  gcOutput.module = ModuleObject::create(cx);
  if (!gcOutput.module) {
    return false;
  }

  Rooted<ModuleObject*> module(cx, gcOutput.module);
  return stencil.moduleMetadata->initModule(cx, input.atomCache, module);
}

// Instantiate JSFunctions for each FunctionBox.
static bool InstantiateFunctions(JSContext* cx, CompilationInput& input,
                                 const CompilationStencil& stencil,
                                 CompilationGCOutput& gcOutput) {
  using ImmutableFlags = ImmutableScriptFlagsEnum;

  if (!gcOutput.functions.resize(stencil.scriptData.size())) {
    ReportOutOfMemory(cx);
    return false;
  }

  // Most JSFunctions will be have the same Shape / Group so we can compute it
  // now to allow fast object creation. Generators / Async will use the slow
  // path instead.
  RootedObject proto(cx,
                     GlobalObject::getOrCreatePrototype(cx, JSProto_Function));
  if (!proto) {
    return false;
  }
  RootedShape shape(
      cx, SharedShape::getInitialShape(cx, &JSFunction::class_, cx->realm(),
                                       TaggedProto(proto),
                                       /* nfixed = */ 0, ObjectFlags()));
  if (!shape) {
    return false;
  }

  for (auto item :
       CompilationStencil::functionScriptStencils(stencil, gcOutput)) {
    const auto& scriptStencil = item.script;
    const auto& scriptExtra = (*item.scriptExtra);
    auto index = item.index;

    MOZ_ASSERT(!item.function);

    // Plain functions can use a fast path.
    bool useFastPath =
        !scriptExtra.immutableFlags.hasFlag(ImmutableFlags::IsAsync) &&
        !scriptExtra.immutableFlags.hasFlag(ImmutableFlags::IsGenerator) &&
        !scriptStencil.functionFlags.isAsmJSNative();

    JSFunction* fun =
        useFastPath
            ? CreateFunctionFast(cx, input, shape, scriptStencil, scriptExtra)
            : CreateFunction(cx, input, stencil, scriptStencil, scriptExtra,
                             index);
    if (!fun) {
      return false;
    }

    // Self-hosted functions may have an canonical name that differs from the
    // function name.  In that case, store this canonical name in an extended
    // slot.
    if (scriptStencil.hasSelfHostedCanonicalName()) {
      JSAtom* canonicalName = input.atomCache.getExistingAtomAt(
          cx, scriptStencil.selfHostedCanonicalName());
      SetUnclonedSelfHostedCanonicalName(fun, canonicalName);
    }

    gcOutput.functions[index] = fun;
  }

  return true;
}

// Instantiate Scope for each ScopeStencil.
//
// This should be called after InstantiateFunctions, given FunctionScope needs
// associated JSFunction pointer, and also should be called before
// InstantiateScriptStencils, given JSScript needs Scope pointer in gc things.
static bool InstantiateScopes(JSContext* cx, CompilationInput& input,
                              const CompilationStencil& stencil,
                              CompilationGCOutput& gcOutput) {
  // While allocating Scope object from ScopeStencil, Scope object for the
  // enclosing Scope should already be allocated.
  //
  // Enclosing scope of ScopeStencil can be either ScopeStencil or Scope*
  // pointer.
  //
  // If the enclosing scope is ScopeStencil, it's guaranteed to be earlier
  // element in stencil.scopeData, because enclosing_ field holds
  // index into it, and newly created ScopeStencil is pushed back to the vector.
  //
  // If the enclosing scope is Scope*, it's CompilationInput.enclosingScope.

  MOZ_ASSERT(stencil.scopeData.size() == stencil.scopeNames.size());
  size_t scopeCount = stencil.scopeData.size();
  for (size_t i = 0; i < scopeCount; i++) {
    Scope* scope = stencil.scopeData[i].createScope(cx, input, gcOutput,
                                                    stencil.scopeNames[i]);
    if (!scope) {
      return false;
    }
    gcOutput.scopes.infallibleAppend(scope);
  }

  return true;
}

// Instantiate js::BaseScripts from ScriptStencils for inner functions of the
// compilation. Note that standalone functions and functions being delazified
// are handled below with other top-levels.
static bool InstantiateScriptStencils(JSContext* cx, CompilationInput& input,
                                      const CompilationStencil& stencil,
                                      CompilationGCOutput& gcOutput) {
  MOZ_ASSERT(stencil.isInitialStencil());

  Rooted<JSFunction*> fun(cx);
  for (auto item :
       CompilationStencil::functionScriptStencils(stencil, gcOutput)) {
    auto& scriptStencil = item.script;
    auto* scriptExtra = item.scriptExtra;
    fun = item.function;
    auto index = item.index;
    if (scriptStencil.hasSharedData()) {
      // If the function was not referenced by enclosing script's bytecode, we
      // do not generate a BaseScript for it. For example, `(function(){});`.
      //
      // `wasEmittedByEnclosingScript` is false also for standalone
      // functions. They are handled in InstantiateTopLevel.
      if (!scriptStencil.wasEmittedByEnclosingScript()) {
        continue;
      }

      RootedScript script(
          cx, JSScript::fromStencil(cx, input, stencil, gcOutput, index));
      if (!script) {
        return false;
      }

      if (scriptStencil.allowRelazify()) {
        MOZ_ASSERT(script->isRelazifiable());
        script->setAllowRelazify();
      }
    } else if (scriptStencil.functionFlags.isAsmJSNative()) {
      MOZ_ASSERT(fun->isAsmJSNative());
    } else {
      MOZ_ASSERT(fun->isIncomplete());
      if (!CreateLazyScript(cx, input, stencil, gcOutput, scriptStencil,
                            *scriptExtra, index, fun)) {
        return false;
      }
    }
  }

  return true;
}

// Instantiate the Stencil for the top-level script of the compilation. This
// includes standalone functions and functions being delazified.
static bool InstantiateTopLevel(JSContext* cx, CompilationInput& input,
                                const CompilationStencil& stencil,
                                CompilationGCOutput& gcOutput) {
  const ScriptStencil& scriptStencil =
      stencil.scriptData[CompilationStencil::TopLevelIndex];

  // Top-level asm.js does not generate a JSScript.
  if (scriptStencil.functionFlags.isAsmJSNative()) {
    return true;
  }

  MOZ_ASSERT(scriptStencil.hasSharedData());
  MOZ_ASSERT(stencil.sharedData.get(CompilationStencil::TopLevelIndex));

  if (!stencil.isInitialStencil()) {
    MOZ_ASSERT(input.lazyOuterScript());
    RootedScript script(cx, JSScript::CastFromLazy(input.lazyOuterScript()));
    if (!JSScript::fullyInitFromStencil(cx, input, stencil, gcOutput, script,
                                        CompilationStencil::TopLevelIndex)) {
      return false;
    }

    if (scriptStencil.allowRelazify()) {
      MOZ_ASSERT(script->isRelazifiable());
      script->setAllowRelazify();
    }

    gcOutput.script = script;
    return true;
  }

  gcOutput.script = JSScript::fromStencil(cx, input, stencil, gcOutput,
                                          CompilationStencil::TopLevelIndex);
  if (!gcOutput.script) {
    return false;
  }

  if (scriptStencil.allowRelazify()) {
    MOZ_ASSERT(gcOutput.script->isRelazifiable());
    gcOutput.script->setAllowRelazify();
  }

  const ScriptStencilExtra& scriptExtra =
      stencil.scriptExtra[CompilationStencil::TopLevelIndex];

  // Finish initializing the ModuleObject if needed.
  if (scriptExtra.isModule()) {
    RootedScript script(cx, gcOutput.script);
    RootedModuleObject module(cx, gcOutput.module);

    script->outermostScope()->as<ModuleScope>().initModule(module);

    module->initScriptSlots(script);
    module->initStatusSlot();

    if (!ModuleObject::createEnvironment(cx, module)) {
      return false;
    }

    // Off-thread compilation with parseGlobal will freeze the module object
    // in GlobalHelperThreadState::finishSingleParseTask instead.
    if (!cx->isHelperThreadContext()) {
      if (!ModuleObject::Freeze(cx, module)) {
        return false;
      }
    }
  }

  return true;
}

// When a function is first referenced by enclosing script's bytecode, we need
// to update it with information determined by the BytecodeEmitter. This applies
// to both initial and delazification parses. The functions being update may or
// may not have bytecode at this point.
static void UpdateEmittedInnerFunctions(JSContext* cx, CompilationInput& input,
                                        const CompilationStencil& stencil,
                                        CompilationGCOutput& gcOutput) {
  for (auto item :
       CompilationStencil::functionScriptStencils(stencil, gcOutput)) {
    auto& scriptStencil = item.script;
    auto& fun = item.function;
    if (!scriptStencil.wasEmittedByEnclosingScript()) {
      continue;
    }

    if (scriptStencil.functionFlags.isAsmJSNative() ||
        fun->baseScript()->hasBytecode()) {
      // Non-lazy inner functions don't use the enclosingScope_ field.
      MOZ_ASSERT(!scriptStencil.hasLazyFunctionEnclosingScopeIndex());
    } else {
      // Apply updates from FunctionEmitter::emitLazy().
      BaseScript* script = fun->baseScript();

      ScopeIndex index = scriptStencil.lazyFunctionEnclosingScopeIndex();
      Scope* scope = gcOutput.scopes[index];
      script->setEnclosingScope(scope);

      // Inferred and Guessed names are computed by BytecodeEmitter and so may
      // need to be applied to existing JSFunctions during delazification.
      if (fun->displayAtom() == nullptr) {
        JSAtom* funcAtom = nullptr;
        if (scriptStencil.functionFlags.hasInferredName() ||
            scriptStencil.functionFlags.hasGuessedAtom()) {
          funcAtom =
              input.atomCache.getExistingAtomAt(cx, scriptStencil.functionAtom);
          MOZ_ASSERT(funcAtom);
        }
        if (scriptStencil.functionFlags.hasInferredName()) {
          fun->setInferredName(funcAtom);
        }
        if (scriptStencil.functionFlags.hasGuessedAtom()) {
          fun->setGuessedAtom(funcAtom);
        }
      }
    }
  }
}

// During initial parse we must link lazy-functions-inside-lazy-functions to
// their enclosing script.
static void LinkEnclosingLazyScript(const CompilationStencil& stencil,
                                    CompilationGCOutput& gcOutput) {
  for (auto item :
       CompilationStencil::functionScriptStencils(stencil, gcOutput)) {
    auto& scriptStencil = item.script;
    auto& fun = item.function;
    if (!scriptStencil.functionFlags.hasBaseScript()) {
      continue;
    }

    if (!fun->baseScript()) {
      continue;
    }

    if (fun->baseScript()->hasBytecode()) {
      continue;
    }

    BaseScript* script = fun->baseScript();
    MOZ_ASSERT(!script->hasBytecode());

    for (auto inner : script->gcthings()) {
      if (!inner.is<JSObject>()) {
        continue;
      }
      JSFunction* innerFun = &inner.as<JSObject>().as<JSFunction>();

      MOZ_ASSERT(innerFun->hasBaseScript(),
                 "inner function should have base script");
      if (!innerFun->hasBaseScript()) {
        continue;
      }

      // Check for the case that the inner function has the base script flag,
      // but still doesn't have the actual base script pointer.
      // `baseScript` method asserts the pointer itself, so no extra MOZ_ASSERT
      // here.
      if (!innerFun->baseScript()) {
        continue;
      }

      innerFun->setEnclosingLazyScript(script);
    }
  }
}

#ifdef DEBUG
// Some fields aren't used in delazification, given the target functions and
// scripts are already instantiated, but they still should match.
static void AssertDelazificationFieldsMatch(const CompilationStencil& stencil,
                                            CompilationGCOutput& gcOutput) {
  for (auto item :
       CompilationStencil::functionScriptStencils(stencil, gcOutput)) {
    auto& scriptStencil = item.script;
    auto* scriptExtra = item.scriptExtra;
    auto& fun = item.function;

    MOZ_ASSERT(scriptExtra == nullptr);

    // Names are updated by UpdateInnerFunctions.
    constexpr uint16_t HAS_INFERRED_NAME =
        uint16_t(FunctionFlags::Flags::HAS_INFERRED_NAME);
    constexpr uint16_t HAS_GUESSED_ATOM =
        uint16_t(FunctionFlags::Flags::HAS_GUESSED_ATOM);
    constexpr uint16_t MUTABLE_FLAGS =
        uint16_t(FunctionFlags::Flags::MUTABLE_FLAGS);
    constexpr uint16_t acceptableDifferenceForFunction =
        HAS_INFERRED_NAME | HAS_GUESSED_ATOM | MUTABLE_FLAGS;

    MOZ_ASSERT((fun->flags().toRaw() | acceptableDifferenceForFunction) ==
               (scriptStencil.functionFlags.toRaw() |
                acceptableDifferenceForFunction));

    // Delazification shouldn't delazify inner scripts.
    MOZ_ASSERT_IF(item.index == CompilationStencil::TopLevelIndex,
                  scriptStencil.hasSharedData());
    MOZ_ASSERT_IF(item.index > CompilationStencil::TopLevelIndex,
                  !scriptStencil.hasSharedData());
  }
}
#endif  // DEBUG

// When delazifying, use the existing JSFunctions. The initial and delazifying
// parse are required to generate the same sequence of functions for lazy
// parsing to work at all.
static void FunctionsFromExistingLazy(CompilationInput& input,
                                      CompilationGCOutput& gcOutput) {
  MOZ_ASSERT(gcOutput.functions.empty());
  gcOutput.functions.infallibleAppend(input.function());

  for (JS::GCCellPtr elem : input.lazyOuterScript()->gcthings()) {
    if (!elem.is<JSObject>()) {
      continue;
    }
    JSFunction* fun = &elem.as<JSObject>().as<JSFunction>();
    gcOutput.functions.infallibleAppend(fun);
  }
}

/* static */
bool CompilationStencil::instantiateStencils(JSContext* cx,
                                             CompilationInput& input,
                                             const CompilationStencil& stencil,
                                             CompilationGCOutput& gcOutput) {
  if (!prepareForInstantiate(cx, input, stencil, gcOutput)) {
    return false;
  }

  return instantiateStencilAfterPreparation(cx, input, stencil, gcOutput);
}

/* static */
bool CompilationStencil::instantiateStencilAfterPreparation(
    JSContext* cx, CompilationInput& input, const CompilationStencil& stencil,
    CompilationGCOutput& gcOutput) {
  // Distinguish between the initial (possibly lazy) compile and any subsequent
  // delazification compiles. Delazification will update existing GC things.
  bool isInitialParse = stencil.isInitialStencil();
  MOZ_ASSERT(stencil.isInitialStencil() == input.isInitialStencil());

  // Phase 1: Instantate JSAtoms.
  if (!InstantiateAtoms(cx, input, stencil)) {
    return false;
  }

  // Phase 2: Instantiate ScriptSourceObject, ModuleObject, JSFunctions.
  if (isInitialParse) {
    if (!InstantiateScriptSourceObject(cx, input, stencil, gcOutput)) {
      return false;
    }

    if (stencil.moduleMetadata) {
      // The enclosing script of a module is always the global scope. Fetch the
      // scope of the current global and update input data.
      MOZ_ASSERT(input.enclosingScope == nullptr);
      input.enclosingScope = &cx->global()->emptyGlobalScope();
      MOZ_ASSERT(input.enclosingScope->environmentChainLength() ==
                 ModuleScope::EnclosingEnvironmentChainLength);

      if (!InstantiateModuleObject(cx, input, stencil, gcOutput)) {
        return false;
      }
    }

    if (!InstantiateFunctions(cx, input, stencil, gcOutput)) {
      return false;
    }
  } else {
    MOZ_ASSERT(
        stencil.scriptData[CompilationStencil::TopLevelIndex].isFunction());

    // FunctionKey is used when caching to map a delazification stencil to a
    // specific lazy script. It is not used by instantiation, but we should
    // ensure it is correctly defined.
    MOZ_ASSERT(stencil.functionKey ==
               CompilationStencil::toFunctionKey(input.extent()));

    FunctionsFromExistingLazy(input, gcOutput);
    MOZ_ASSERT(gcOutput.functions.length() == stencil.scriptData.size());

#ifdef DEBUG
    AssertDelazificationFieldsMatch(stencil, gcOutput);
#endif
  }

  // Phase 3: Instantiate js::Scopes.
  if (!InstantiateScopes(cx, input, stencil, gcOutput)) {
    return false;
  }

  // Phase 4: Instantiate (inner) BaseScripts.
  if (isInitialParse) {
    if (!InstantiateScriptStencils(cx, input, stencil, gcOutput)) {
      return false;
    }
  }

  // Phase 5: Finish top-level handling
  if (!InstantiateTopLevel(cx, input, stencil, gcOutput)) {
    return false;
  }

  // !! Must be infallible from here forward !!

  // Phase 6: Update lazy scripts.
  MOZ_ASSERT(stencil.canLazilyParse == CanLazilyParse(input.options));
  if (stencil.canLazilyParse) {
    UpdateEmittedInnerFunctions(cx, input, stencil, gcOutput);

    if (isInitialParse) {
      LinkEnclosingLazyScript(stencil, gcOutput);
    }
  }

  return true;
}

/* static */
bool CompilationStencil::prepareForInstantiate(
    JSContext* cx, CompilationInput& input, const CompilationStencil& stencil,
    CompilationGCOutput& gcOutput) {
  // Reserve the `gcOutput` vectors.
  if (!gcOutput.ensureReserved(cx, stencil.scriptData.size(),
                               stencil.scopeData.size())) {
    return false;
  }

  return input.atomCache.allocate(cx, stencil.parserAtomData.size());
}

bool CompilationStencil::serializeStencils(JSContext* cx,
                                           CompilationInput& input,
                                           JS::TranscodeBuffer& buf,
                                           bool* succeededOut) const {
  if (succeededOut) {
    *succeededOut = false;
  }
  XDRStencilEncoder encoder(cx, buf);

  XDRResult res = encoder.codeStencil(input, *this);
  if (res.isErr()) {
    if (JS::IsTranscodeFailureResult(res.unwrapErr())) {
      buf.clear();
      return true;
    }
    MOZ_ASSERT(res.unwrapErr() == JS::TranscodeResult::Throw);

    return false;
  }

  if (succeededOut) {
    *succeededOut = true;
  }
  return true;
}

bool CompilationStencil::deserializeStencils(JSContext* cx,
                                             CompilationInput& input,
                                             const JS::TranscodeRange& range,
                                             bool* succeededOut) {
  if (succeededOut) {
    *succeededOut = false;
  }
  MOZ_ASSERT(parserAtomData.empty());
  XDRStencilDecoder decoder(cx, range);

  XDRResult res = decoder.codeStencil(input, *this);
  if (res.isErr()) {
    if (JS::IsTranscodeFailureResult(res.unwrapErr())) {
      return true;
    }
    MOZ_ASSERT(res.unwrapErr() == JS::TranscodeResult::Throw);

    return false;
  }

  if (succeededOut) {
    *succeededOut = true;
  }
  return true;
}

ExtensibleCompilationStencil::ExtensibleCompilationStencil(
    JSContext* cx, CompilationInput& input)
    : canLazilyParse(CanLazilyParse(input.options)),
      alloc(CompilationStencil::LifoAllocChunkSize),
      source(input.source),
      parserAtoms(cx->runtime(), alloc) {}

CompilationState::CompilationState(JSContext* cx,
                                   LifoAllocScope& frontendAllocScope,
                                   CompilationInput& input)
    : ExtensibleCompilationStencil(cx, input),
      directives(input.options.forceStrictMode()),
      usedNames(cx),
      allocScope(frontendAllocScope),
      input(input) {}

BorrowingCompilationStencil::BorrowingCompilationStencil(
    ExtensibleCompilationStencil& extensibleStencil)
    : CompilationStencil(extensibleStencil.source) {
  hasExternalDependency = true;

  canLazilyParse = extensibleStencil.canLazilyParse;
  functionKey = extensibleStencil.functionKey;

  // Borrow the vector content as span.
  scriptData = extensibleStencil.scriptData;
  scriptExtra = extensibleStencil.scriptExtra;

  gcThingData = extensibleStencil.gcThingData;

  scopeData = extensibleStencil.scopeData;
  scopeNames = extensibleStencil.scopeNames;

  regExpData = extensibleStencil.regExpData;
  bigIntData = extensibleStencil.bigIntData;
  objLiteralData = extensibleStencil.objLiteralData;

  // Borrow the parser atoms as span.
  parserAtomData = extensibleStencil.parserAtoms.entries_;

  // Borrow container.
  sharedData.setBorrow(&extensibleStencil.sharedData);

  // Share ref-counted data.
  source = extensibleStencil.source;
  asmJS = extensibleStencil.asmJS;
  moduleMetadata = extensibleStencil.moduleMetadata;
}

SharedDataContainer::~SharedDataContainer() {
  if (isEmpty()) {
    // Nothing to do.
  } else if (isSingle()) {
    asSingle()->Release();
  } else if (isVector()) {
    js_delete(asVector());
  } else if (isMap()) {
    js_delete(asMap());
  } else {
    MOZ_ASSERT(isBorrow());
    // Nothing to do.
  }
}

bool SharedDataContainer::initVector(JSContext* cx) {
  MOZ_ASSERT(isEmpty());

  auto* vec = js_new<SharedDataVector>();
  if (!vec) {
    ReportOutOfMemory(cx);
    return false;
  }
  data_ = uintptr_t(vec) | VectorTag;
  return true;
}

bool SharedDataContainer::initMap(JSContext* cx) {
  MOZ_ASSERT(isEmpty());

  auto* map = js_new<SharedDataMap>();
  if (!map) {
    ReportOutOfMemory(cx);
    return false;
  }
  data_ = uintptr_t(map) | MapTag;
  return true;
}

bool SharedDataContainer::prepareStorageFor(JSContext* cx,
                                            size_t nonLazyScriptCount,
                                            size_t allScriptCount) {
  MOZ_ASSERT(isEmpty());

  if (nonLazyScriptCount <= 1) {
    MOZ_ASSERT(isSingle());
    return true;
  }

  // If the ratio of scripts with bytecode is small, allocating the Vector
  // storage with the number of all scripts isn't space-efficient.
  // In that case use HashMap instead.
  //
  // In general, we expect either all scripts to contain bytecode (priviledge
  // and self-hosted), or almost none to (eg standard lazy parsing output).
  constexpr size_t thresholdRatio = 8;
  bool useHashMap = nonLazyScriptCount < allScriptCount / thresholdRatio;
  if (useHashMap) {
    if (!initMap(cx)) {
      return false;
    }
    if (!asMap()->reserve(nonLazyScriptCount)) {
      ReportOutOfMemory(cx);
      return false;
    }
  } else {
    if (!initVector(cx)) {
      return false;
    }
    if (!asVector()->resize(allScriptCount)) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

js::SharedImmutableScriptData* SharedDataContainer::get(
    ScriptIndex index) const {
  if (isSingle()) {
    if (index == CompilationStencil::TopLevelIndex) {
      return asSingle();
    }
    return nullptr;
  }

  if (isVector()) {
    auto& vec = *asVector();
    if (index.index < vec.length()) {
      return vec[index];
    }
    return nullptr;
  }

  if (isMap()) {
    auto& map = *asMap();
    auto p = map.lookup(index);
    if (p) {
      return p->value();
    }
    return nullptr;
  }

  MOZ_ASSERT(isBorrow());
  return asBorrow()->get(index);
}

bool SharedDataContainer::convertFromSingleToMap(JSContext* cx) {
  MOZ_ASSERT(isSingle());

  // Use a temporary container so that on OOM we do not break the stencil.
  SharedDataContainer other;
  if (!other.initMap(cx)) {
    return false;
  }

  if (!other.asMap()->putNew(CompilationStencil::TopLevelIndex, asSingle())) {
    ReportOutOfMemory(cx);
    return false;
  }

  std::swap(data_, other.data_);
  return true;
}

bool SharedDataContainer::addAndShare(JSContext* cx, ScriptIndex index,
                                      js::SharedImmutableScriptData* data) {
  MOZ_ASSERT(!isBorrow());

  if (isSingle()) {
    MOZ_ASSERT(index == CompilationStencil::TopLevelIndex);
    RefPtr<SharedImmutableScriptData> ref(data);
    if (!SharedImmutableScriptData::shareScriptData(cx, ref)) {
      return false;
    }
    setSingle(ref.forget());
    return true;
  }

  if (isVector()) {
    auto& vec = *asVector();
    // Resized by SharedDataContainer::prepareStorageFor.
    vec[index] = data;
    return SharedImmutableScriptData::shareScriptData(cx, vec[index]);
  }

  MOZ_ASSERT(isMap());
  auto& map = *asMap();
  // Reserved by SharedDataContainer::prepareStorageFor.
  map.putNewInfallible(index, data);
  auto p = map.lookup(index);
  MOZ_ASSERT(p);
  return SharedImmutableScriptData::shareScriptData(cx, p->value());
}

bool SharedDataContainer::addExtraWithoutShare(
    JSContext* cx, ScriptIndex index, js::SharedImmutableScriptData* data) {
  MOZ_ASSERT(!isEmpty());

  if (isSingle()) {
    if (!convertFromSingleToMap(cx)) {
      return false;
    }
  }

  if (isVector()) {
    // SharedDataContainer::prepareStorageFor allocates space for all scripts.
    (*asVector())[index] = data;
    return true;
  }

  MOZ_ASSERT(isMap());
  // SharedDataContainer::prepareStorageFor doesn't allocate space for
  // delazification, and this can fail.
  if (!asMap()->putNew(index, data)) {
    ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

#ifdef DEBUG
void CompilationStencil::assertNoExternalDependency() const {
  MOZ_ASSERT_IF(!scriptData.empty(), alloc.contains(scriptData.data()));
  MOZ_ASSERT_IF(!scriptExtra.empty(), alloc.contains(scriptExtra.data()));

  MOZ_ASSERT_IF(!scopeData.empty(), alloc.contains(scopeData.data()));
  MOZ_ASSERT_IF(!scopeNames.empty(), alloc.contains(scopeNames.data()));
  for (const auto* data : scopeNames) {
    MOZ_ASSERT_IF(data, alloc.contains(data));
  }

  MOZ_ASSERT_IF(!regExpData.empty(), alloc.contains(regExpData.data()));

  MOZ_ASSERT_IF(!bigIntData.empty(), alloc.contains(bigIntData.data()));
  for (const auto& data : bigIntData) {
    MOZ_ASSERT(data.isContainedIn(alloc));
  }

  MOZ_ASSERT_IF(!objLiteralData.empty(), alloc.contains(objLiteralData.data()));
  for (const auto& data : objLiteralData) {
    MOZ_ASSERT(data.isContainedIn(alloc));
  }

  MOZ_ASSERT_IF(!parserAtomData.empty(), alloc.contains(parserAtomData.data()));
  for (const auto* data : parserAtomData) {
    MOZ_ASSERT_IF(data, alloc.contains(data));
  }

  MOZ_ASSERT(!sharedData.isBorrow());
}

void ExtensibleCompilationStencil::assertNoExternalDependency() const {
  for (const auto& data : bigIntData) {
    MOZ_ASSERT(data.isContainedIn(alloc));
  }

  for (const auto& data : objLiteralData) {
    MOZ_ASSERT(data.isContainedIn(alloc));
  }

  for (const auto* data : scopeNames) {
    MOZ_ASSERT_IF(data, alloc.contains(data));
  }

  for (const auto* data : parserAtoms.entries()) {
    MOZ_ASSERT_IF(data, alloc.contains(data));
  }

  MOZ_ASSERT(!sharedData.isBorrow());
}
#endif  // DEBUG

template <typename T, typename VectorT>
bool CopyVectorToSpan(JSContext* cx, LifoAlloc& alloc, mozilla::Span<T>& span,
                      VectorT& vec) {
  auto len = vec.length();
  if (len == 0) {
    return true;
  }

  auto* p = alloc.newArrayUninitialized<T>(len);
  if (!p) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  span = mozilla::Span(p, len);
  memcpy(span.data(), vec.begin(), sizeof(T) * len);
  return true;
}

bool CompilationStencil::steal(JSContext* cx,
                               ExtensibleCompilationStencil&& other) {
#ifdef DEBUG
  other.assertNoExternalDependency();
#endif

  canLazilyParse = other.canLazilyParse;
  functionKey = other.functionKey;

  MOZ_ASSERT(alloc.isEmpty());
  alloc.steal(&other.alloc);

  if (!CopyVectorToSpan(cx, alloc, scriptData, other.scriptData)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, scriptExtra, other.scriptExtra)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, gcThingData, other.gcThingData)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, scopeData, other.scopeData)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, scopeNames, other.scopeNames)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, regExpData, other.regExpData)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, bigIntData, other.bigIntData)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, objLiteralData, other.objLiteralData)) {
    return false;
  }

  if (!CopyVectorToSpan(cx, alloc, parserAtomData,
                        other.parserAtoms.entries())) {
    return false;
  }

  sharedData = std::move(other.sharedData);

  moduleMetadata = std::move(other.moduleMetadata);

  asmJS = std::move(other.asmJS);

#ifdef DEBUG
  assertNoExternalDependency();
#endif

  return true;
}

template <typename T, typename VectorT>
[[nodiscard]] bool CopySpanToVector(JSContext* cx, VectorT& vec,
                                    mozilla::Span<T>& span) {
  auto len = span.size();
  if (len == 0) {
    return true;
  }

  if (!vec.append(span.data(), len)) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  return true;
}

// Copy scope names from `src` into `alloc`, and returns the allocated data.
BaseParserScopeData* CopyScopeData(JSContext* cx, LifoAlloc& alloc,
                                   ScopeKind kind,
                                   const BaseParserScopeData* src) {
  MOZ_ASSERT(kind != ScopeKind::With);

  size_t dataSize = SizeOfParserScopeData(kind, src->length);

  auto* dest = static_cast<BaseParserScopeData*>(alloc.alloc(dataSize));
  if (!dest) {
    js::ReportOutOfMemory(cx);
    return nullptr;
  }
  memcpy(dest, src, dataSize);

  return dest;
}

bool ExtensibleCompilationStencil::steal(JSContext* cx,
                                         CompilationStencil&& other) {
  MOZ_ASSERT(alloc.isEmpty());

  canLazilyParse = other.canLazilyParse;
  functionKey = other.functionKey;

  if (!other.hasExternalDependency) {
#ifdef DEBUG
    other.assertNoExternalDependency();
#endif

    // If CompilationStencil has no external dependency,
    // steal LifoAlloc and perform shallow copy.
    alloc.steal(&other.alloc);
  }

  if (!CopySpanToVector(cx, scriptData, other.scriptData)) {
    return false;
  }

  if (!CopySpanToVector(cx, scriptExtra, other.scriptExtra)) {
    return false;
  }

  if (!CopySpanToVector(cx, gcThingData, other.gcThingData)) {
    return false;
  }

  if (other.hasExternalDependency) {
    size_t scopeSize = other.scopeData.size();

    if (!CopySpanToVector(cx, scopeData, other.scopeData)) {
      return false;
    }
    if (!scopeNames.reserve(scopeSize)) {
      js::ReportOutOfMemory(cx);
      return false;
    }
    for (size_t i = 0; i < scopeSize; i++) {
      if (other.scopeNames[i]) {
        BaseParserScopeData* data = CopyScopeData(
            cx, alloc, other.scopeData[i].kind(), other.scopeNames[i]);
        if (!data) {
          return false;
        }
        scopeNames.infallibleEmplaceBack(data);
      } else {
        scopeNames.infallibleEmplaceBack(nullptr);
      }
    }
  } else {
    if (!CopySpanToVector(cx, scopeData, other.scopeData)) {
      return false;
    }
    if (!CopySpanToVector(cx, scopeNames, other.scopeNames)) {
      return false;
    }
  }

  if (!CopySpanToVector(cx, regExpData, other.regExpData)) {
    return false;
  }

  if (other.hasExternalDependency) {
    // If CompilationStencil has external dependency, peform deep copy.

    size_t bigIntSize = other.bigIntData.size();
    if (!bigIntData.resize(bigIntSize)) {
      js::ReportOutOfMemory(cx);
      return false;
    }
    for (size_t i = 0; i < bigIntSize; i++) {
      if (!bigIntData[i].init(cx, alloc, other.bigIntData[i].source())) {
        return false;
      }
    }
  } else {
    if (!CopySpanToVector(cx, bigIntData, other.bigIntData)) {
      return false;
    }
  }

  if (other.hasExternalDependency) {
    size_t objLiteralSize = other.objLiteralData.size();
    if (!objLiteralData.reserve(objLiteralSize)) {
      js::ReportOutOfMemory(cx);
      return false;
    }
    for (const auto& data : other.objLiteralData) {
      size_t length = data.code().size();
      auto* code = alloc.newArrayUninitialized<uint8_t>(length);
      if (!code) {
        js::ReportOutOfMemory(cx);
        return false;
      }
      memcpy(code, data.code().data(), length);
      objLiteralData.infallibleEmplaceBack(code, length, data.flags(),
                                           data.propertyCount());
    }
  } else {
    if (!CopySpanToVector(cx, objLiteralData, other.objLiteralData)) {
      return false;
    }
  }

  // Regardless of whether CompilationStencil has external dependency or not,
  // ParserAtoms should be interned, to populate internal HashMap.
  for (const auto* entry : other.parserAtomData) {
    if (!entry) {
      if (!parserAtoms.addPlaceholder(cx)) {
        return false;
      }
      continue;
    }

    auto index = parserAtoms.internExternalParserAtom(cx, entry);
    if (!index) {
      return false;
    }
    if (entry->isUsedByStencil()) {
      parserAtoms.markUsedByStencil(index);
    }
  }

  sharedData = std::move(other.sharedData);

  moduleMetadata = std::move(other.moduleMetadata);

  asmJS = std::move(other.asmJS);

#ifdef DEBUG
  assertNoExternalDependency();
#endif

  return true;
}

mozilla::Span<TaggedScriptThingIndex> ScriptStencil::gcthings(
    const CompilationStencil& stencil) const {
  return stencil.gcThingData.Subspan(gcThingsOffset, gcThingsLength);
}

bool BigIntStencil::init(JSContext* cx, LifoAlloc& alloc,
                         const mozilla::Span<const char16_t> buf) {
#ifdef DEBUG
  // Assert we have no separators; if we have a separator then the algorithm
  // used in BigInt::literalIsZero will be incorrect.
  for (char16_t c : buf) {
    MOZ_ASSERT(c != '_');
  }
#endif
  size_t length = buf.size();
  char16_t* p = alloc.template newArrayUninitialized<char16_t>(length);
  if (!p) {
    ReportOutOfMemory(cx);
    return false;
  }
  mozilla::PodCopy(p, buf.data(), length);
  source_ = mozilla::Span(p, length);
  return true;
}

#ifdef DEBUG
bool BigIntStencil::isContainedIn(const LifoAlloc& alloc) const {
  return alloc.contains(source_.data());
}
#endif

#if defined(DEBUG) || defined(JS_JITSPEW)

void frontend::DumpTaggedParserAtomIndex(js::JSONPrinter& json,
                                         TaggedParserAtomIndex taggedIndex,
                                         const CompilationStencil* stencil) {
  if (taggedIndex.isParserAtomIndex()) {
    json.property("tag", "AtomIndex");
    auto index = taggedIndex.toParserAtomIndex();
    if (stencil && stencil->parserAtomData[index]) {
      GenericPrinter& out = json.beginStringProperty("atom");
      stencil->parserAtomData[index]->dumpCharsNoQuote(out);
      json.endString();
    } else {
      json.property("index", size_t(index));
    }
    return;
  }

  if (taggedIndex.isWellKnownAtomId()) {
    json.property("tag", "WellKnown");
    auto index = taggedIndex.toWellKnownAtomId();
    switch (index) {
      case WellKnownAtomId::empty:
        json.property("atom", "");
        break;

#  define CASE_(_, name, _2) case WellKnownAtomId::name:
        FOR_EACH_NONTINY_COMMON_PROPERTYNAME(CASE_)
#  undef CASE_

#  define CASE_(name, _) case WellKnownAtomId::name:
        JS_FOR_EACH_PROTOTYPE(CASE_)
#  undef CASE_

#  define CASE_(name) case WellKnownAtomId::name:
        JS_FOR_EACH_WELL_KNOWN_SYMBOL(CASE_)
#  undef CASE_

        {
          GenericPrinter& out = json.beginStringProperty("atom");
          ParserAtomsTable::dumpCharsNoQuote(out, index);
          json.endString();
          break;
        }

      default:
        // This includes tiny WellKnownAtomId atoms, which is invalid.
        json.property("index", size_t(index));
        break;
    }
    return;
  }

  if (taggedIndex.isLength1StaticParserString()) {
    json.property("tag", "Length1Static");
    auto index = taggedIndex.toLength1StaticParserString();
    GenericPrinter& out = json.beginStringProperty("atom");
    ParserAtomsTable::dumpCharsNoQuote(out, index);
    json.endString();
    return;
  }

  if (taggedIndex.isLength2StaticParserString()) {
    json.property("tag", "Length2Static");
    auto index = taggedIndex.toLength2StaticParserString();
    GenericPrinter& out = json.beginStringProperty("atom");
    ParserAtomsTable::dumpCharsNoQuote(out, index);
    json.endString();
    return;
  }

  MOZ_ASSERT(taggedIndex.isNull());
  json.property("tag", "null");
}

void frontend::DumpTaggedParserAtomIndexNoQuote(
    GenericPrinter& out, TaggedParserAtomIndex taggedIndex,
    const CompilationStencil* stencil) {
  if (taggedIndex.isParserAtomIndex()) {
    auto index = taggedIndex.toParserAtomIndex();
    if (stencil && stencil->parserAtomData[index]) {
      stencil->parserAtomData[index]->dumpCharsNoQuote(out);
    } else {
      out.printf("AtomIndex#%zu", size_t(index));
    }
    return;
  }

  if (taggedIndex.isWellKnownAtomId()) {
    auto index = taggedIndex.toWellKnownAtomId();
    switch (index) {
      case WellKnownAtomId::empty:
        out.put("#<zero-length name>");
        break;

#  define CASE_(_, name, _2) case WellKnownAtomId::name:
        FOR_EACH_NONTINY_COMMON_PROPERTYNAME(CASE_)
#  undef CASE_

#  define CASE_(name, _) case WellKnownAtomId::name:
        JS_FOR_EACH_PROTOTYPE(CASE_)
#  undef CASE_

#  define CASE_(name) case WellKnownAtomId::name:
        JS_FOR_EACH_WELL_KNOWN_SYMBOL(CASE_)
#  undef CASE_

        {
          ParserAtomsTable::dumpCharsNoQuote(out, index);
          break;
        }

      default:
        // This includes tiny WellKnownAtomId atoms, which is invalid.
        out.printf("WellKnown#%zu", size_t(index));
        break;
    }
    return;
  }

  if (taggedIndex.isLength1StaticParserString()) {
    auto index = taggedIndex.toLength1StaticParserString();
    ParserAtomsTable::dumpCharsNoQuote(out, index);
    return;
  }

  if (taggedIndex.isLength2StaticParserString()) {
    auto index = taggedIndex.toLength2StaticParserString();
    ParserAtomsTable::dumpCharsNoQuote(out, index);
    return;
  }

  MOZ_ASSERT(taggedIndex.isNull());
  out.put("#<null name>");
}

void RegExpStencil::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json, nullptr);
}

void RegExpStencil::dump(js::JSONPrinter& json,
                         const CompilationStencil* stencil) const {
  json.beginObject();
  dumpFields(json, stencil);
  json.endObject();
}

void RegExpStencil::dumpFields(js::JSONPrinter& json,
                               const CompilationStencil* stencil) const {
  json.beginObjectProperty("pattern");
  DumpTaggedParserAtomIndex(json, atom_, stencil);
  json.endObject();

  GenericPrinter& out = json.beginStringProperty("flags");

  if (flags().global()) {
    out.put("g");
  }
  if (flags().ignoreCase()) {
    out.put("i");
  }
  if (flags().multiline()) {
    out.put("m");
  }
  if (flags().dotAll()) {
    out.put("s");
  }
  if (flags().unicode()) {
    out.put("u");
  }
  if (flags().sticky()) {
    out.put("y");
  }

  json.endStringProperty();
}

void BigIntStencil::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json);
}

void BigIntStencil::dump(js::JSONPrinter& json) const {
  GenericPrinter& out = json.beginString();
  dumpCharsNoQuote(out);
  json.endString();
}

void BigIntStencil::dumpCharsNoQuote(GenericPrinter& out) const {
  for (char16_t c : source_) {
    out.putChar(char(c));
  }
}

void ScopeStencil::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json, nullptr, nullptr);
}

void ScopeStencil::dump(js::JSONPrinter& json,
                        const BaseParserScopeData* baseScopeData,
                        const CompilationStencil* stencil) const {
  json.beginObject();
  dumpFields(json, baseScopeData, stencil);
  json.endObject();
}

void ScopeStencil::dumpFields(js::JSONPrinter& json,
                              const BaseParserScopeData* baseScopeData,
                              const CompilationStencil* stencil) const {
  json.property("kind", ScopeKindString(kind_));

  if (hasEnclosing()) {
    json.formatProperty("enclosing", "ScopeIndex(%zu)", size_t(enclosing()));
  }

  json.property("firstFrameSlot", firstFrameSlot_);

  if (hasEnvironmentShape()) {
    json.formatProperty("numEnvironmentSlots", "%zu",
                        size_t(numEnvironmentSlots_));
  }

  if (isFunction()) {
    json.formatProperty("functionIndex", "ScriptIndex(%zu)",
                        size_t(functionIndex_));
  }

  json.beginListProperty("flags");
  if (flags_ & HasEnclosing) {
    json.value("HasEnclosing");
  }
  if (flags_ & HasEnvironmentShape) {
    json.value("HasEnvironmentShape");
  }
  if (flags_ & IsArrow) {
    json.value("IsArrow");
  }
  json.endList();

  if (!baseScopeData) {
    return;
  }

  json.beginObjectProperty("data");

  mozilla::Span<const ParserBindingName> trailingNames;
  switch (kind_) {
    case ScopeKind::Function: {
      const auto* data =
          static_cast<const FunctionScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);
      json.property("hasParameterExprs", data->slotInfo.hasParameterExprs());
      json.property("nonPositionalFormalStart",
                    data->slotInfo.nonPositionalFormalStart);
      json.property("varStart", data->slotInfo.varStart);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::FunctionBodyVar: {
      const auto* data =
          static_cast<const VarScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical: {
      const auto* data =
          static_cast<const LexicalScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);
      json.property("constStart", data->slotInfo.constStart);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::ClassBody: {
      const auto* data =
          static_cast<const ClassBodyScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);
      json.property("privateMethodStart", data->slotInfo.privateMethodStart);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::With: {
      break;
    }

    case ScopeKind::Eval:
    case ScopeKind::StrictEval: {
      const auto* data =
          static_cast<const EvalScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::Global:
    case ScopeKind::NonSyntactic: {
      const auto* data =
          static_cast<const GlobalScope::ParserData*>(baseScopeData);
      json.property("letStart", data->slotInfo.letStart);
      json.property("constStart", data->slotInfo.constStart);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::Module: {
      const auto* data =
          static_cast<const ModuleScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);
      json.property("varStart", data->slotInfo.varStart);
      json.property("letStart", data->slotInfo.letStart);
      json.property("constStart", data->slotInfo.constStart);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::WasmInstance: {
      const auto* data =
          static_cast<const WasmInstanceScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);
      json.property("globalsStart", data->slotInfo.globalsStart);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    case ScopeKind::WasmFunction: {
      const auto* data =
          static_cast<const WasmFunctionScope::ParserData*>(baseScopeData);
      json.property("nextFrameSlot", data->slotInfo.nextFrameSlot);

      trailingNames = GetScopeDataTrailingNames(data);
      break;
    }

    default: {
      MOZ_CRASH("Unexpected ScopeKind");
      break;
    }
  }

  if (!trailingNames.empty()) {
    char index[64];
    json.beginObjectProperty("trailingNames");
    for (size_t i = 0; i < trailingNames.size(); i++) {
      const auto& name = trailingNames[i];
      SprintfLiteral(index, "%zu", i);
      json.beginObjectProperty(index);

      json.boolProperty("closedOver", name.closedOver());

      json.boolProperty("isTopLevelFunction", name.isTopLevelFunction());

      json.beginObjectProperty("name");
      DumpTaggedParserAtomIndex(json, name.name(), stencil);
      json.endObject();

      json.endObject();
    }
    json.endObject();
  }

  json.endObject();
}

static void DumpModuleEntryVectorItems(
    js::JSONPrinter& json, const StencilModuleMetadata::EntryVector& entries,
    const CompilationStencil* stencil) {
  for (const auto& entry : entries) {
    json.beginObject();
    if (entry.specifier) {
      json.beginObjectProperty("specifier");
      DumpTaggedParserAtomIndex(json, entry.specifier, stencil);
      json.endObject();
    }
    if (entry.localName) {
      json.beginObjectProperty("localName");
      DumpTaggedParserAtomIndex(json, entry.localName, stencil);
      json.endObject();
    }
    if (entry.importName) {
      json.beginObjectProperty("importName");
      DumpTaggedParserAtomIndex(json, entry.importName, stencil);
      json.endObject();
    }
    if (entry.exportName) {
      json.beginObjectProperty("exportName");
      DumpTaggedParserAtomIndex(json, entry.exportName, stencil);
      json.endObject();
    }
    json.endObject();
  }
}

void StencilModuleMetadata::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json, nullptr);
}

void StencilModuleMetadata::dump(js::JSONPrinter& json,
                                 const CompilationStencil* stencil) const {
  json.beginObject();
  dumpFields(json, stencil);
  json.endObject();
}

void StencilModuleMetadata::dumpFields(
    js::JSONPrinter& json, const CompilationStencil* stencil) const {
  json.beginListProperty("requestedModules");
  DumpModuleEntryVectorItems(json, requestedModules, stencil);
  json.endList();

  json.beginListProperty("importEntries");
  DumpModuleEntryVectorItems(json, importEntries, stencil);
  json.endList();

  json.beginListProperty("localExportEntries");
  DumpModuleEntryVectorItems(json, localExportEntries, stencil);
  json.endList();

  json.beginListProperty("indirectExportEntries");
  DumpModuleEntryVectorItems(json, indirectExportEntries, stencil);
  json.endList();

  json.beginListProperty("starExportEntries");
  DumpModuleEntryVectorItems(json, starExportEntries, stencil);
  json.endList();

  json.beginListProperty("functionDecls");
  for (const auto& index : functionDecls) {
    json.value("ScriptIndex(%zu)", size_t(index));
  }
  json.endList();

  json.boolProperty("isAsync", isAsync);
}

static void DumpImmutableScriptFlags(js::JSONPrinter& json,
                                     ImmutableScriptFlags immutableFlags) {
  for (uint32_t i = 1; i; i = i << 1) {
    if (uint32_t(immutableFlags) & i) {
      switch (ImmutableScriptFlagsEnum(i)) {
        case ImmutableScriptFlagsEnum::IsForEval:
          json.value("IsForEval");
          break;
        case ImmutableScriptFlagsEnum::IsModule:
          json.value("IsModule");
          break;
        case ImmutableScriptFlagsEnum::IsFunction:
          json.value("IsFunction");
          break;
        case ImmutableScriptFlagsEnum::SelfHosted:
          json.value("SelfHosted");
          break;
        case ImmutableScriptFlagsEnum::ForceStrict:
          json.value("ForceStrict");
          break;
        case ImmutableScriptFlagsEnum::HasNonSyntacticScope:
          json.value("HasNonSyntacticScope");
          break;
        case ImmutableScriptFlagsEnum::NoScriptRval:
          json.value("NoScriptRval");
          break;
        case ImmutableScriptFlagsEnum::TreatAsRunOnce:
          json.value("TreatAsRunOnce");
          break;
        case ImmutableScriptFlagsEnum::Strict:
          json.value("Strict");
          break;
        case ImmutableScriptFlagsEnum::HasModuleGoal:
          json.value("HasModuleGoal");
          break;
        case ImmutableScriptFlagsEnum::HasInnerFunctions:
          json.value("HasInnerFunctions");
          break;
        case ImmutableScriptFlagsEnum::HasDirectEval:
          json.value("HasDirectEval");
          break;
        case ImmutableScriptFlagsEnum::BindingsAccessedDynamically:
          json.value("BindingsAccessedDynamically");
          break;
        case ImmutableScriptFlagsEnum::HasCallSiteObj:
          json.value("HasCallSiteObj");
          break;
        case ImmutableScriptFlagsEnum::IsAsync:
          json.value("IsAsync");
          break;
        case ImmutableScriptFlagsEnum::IsGenerator:
          json.value("IsGenerator");
          break;
        case ImmutableScriptFlagsEnum::FunHasExtensibleScope:
          json.value("FunHasExtensibleScope");
          break;
        case ImmutableScriptFlagsEnum::FunctionHasThisBinding:
          json.value("FunctionHasThisBinding");
          break;
        case ImmutableScriptFlagsEnum::NeedsHomeObject:
          json.value("NeedsHomeObject");
          break;
        case ImmutableScriptFlagsEnum::IsDerivedClassConstructor:
          json.value("IsDerivedClassConstructor");
          break;
        case ImmutableScriptFlagsEnum::IsSyntheticFunction:
          json.value("IsSyntheticFunction");
          break;
        case ImmutableScriptFlagsEnum::UseMemberInitializers:
          json.value("UseMemberInitializers");
          break;
        case ImmutableScriptFlagsEnum::HasRest:
          json.value("HasRest");
          break;
        case ImmutableScriptFlagsEnum::NeedsFunctionEnvironmentObjects:
          json.value("NeedsFunctionEnvironmentObjects");
          break;
        case ImmutableScriptFlagsEnum::FunctionHasExtraBodyVarScope:
          json.value("FunctionHasExtraBodyVarScope");
          break;
        case ImmutableScriptFlagsEnum::ShouldDeclareArguments:
          json.value("ShouldDeclareArguments");
          break;
        case ImmutableScriptFlagsEnum::NeedsArgsObj:
          json.value("NeedsArgsObj");
          break;
        case ImmutableScriptFlagsEnum::HasMappedArgsObj:
          json.value("HasMappedArgsObj");
          break;
        default:
          json.value("Unknown(%x)", i);
          break;
      }
    }
  }
}

static void DumpFunctionFlagsItems(js::JSONPrinter& json,
                                   FunctionFlags functionFlags) {
  switch (functionFlags.kind()) {
    case FunctionFlags::FunctionKind::NormalFunction:
      json.value("NORMAL_KIND");
      break;
    case FunctionFlags::FunctionKind::AsmJS:
      json.value("ASMJS_KIND");
      break;
    case FunctionFlags::FunctionKind::Wasm:
      json.value("WASM_KIND");
      break;
    case FunctionFlags::FunctionKind::Arrow:
      json.value("ARROW_KIND");
      break;
    case FunctionFlags::FunctionKind::Method:
      json.value("METHOD_KIND");
      break;
    case FunctionFlags::FunctionKind::ClassConstructor:
      json.value("CLASSCONSTRUCTOR_KIND");
      break;
    case FunctionFlags::FunctionKind::Getter:
      json.value("GETTER_KIND");
      break;
    case FunctionFlags::FunctionKind::Setter:
      json.value("SETTER_KIND");
      break;
    default:
      json.value("Unknown(%x)", uint8_t(functionFlags.kind()));
      break;
  }

  static_assert(FunctionFlags::FUNCTION_KIND_MASK == 0x0007,
                "FunctionKind should use the lowest 3 bits");
  for (uint16_t i = 1 << 3; i; i = i << 1) {
    if (functionFlags.toRaw() & i) {
      switch (FunctionFlags::Flags(i)) {
        case FunctionFlags::Flags::EXTENDED:
          json.value("EXTENDED");
          break;
        case FunctionFlags::Flags::SELF_HOSTED:
          json.value("SELF_HOSTED");
          break;
        case FunctionFlags::Flags::BASESCRIPT:
          json.value("BASESCRIPT");
          break;
        case FunctionFlags::Flags::SELFHOSTLAZY:
          json.value("SELFHOSTLAZY");
          break;
        case FunctionFlags::Flags::CONSTRUCTOR:
          json.value("CONSTRUCTOR");
          break;
        case FunctionFlags::Flags::BOUND_FUN:
          json.value("BOUND_FUN");
          break;
        case FunctionFlags::Flags::LAMBDA:
          json.value("LAMBDA");
          break;
        case FunctionFlags::Flags::WASM_JIT_ENTRY:
          json.value("WASM_JIT_ENTRY");
          break;
        case FunctionFlags::Flags::HAS_INFERRED_NAME:
          json.value("HAS_INFERRED_NAME");
          break;
        case FunctionFlags::Flags::ATOM_EXTRA_FLAG:
          json.value("ATOM_EXTRA_FLAG");
          break;
        case FunctionFlags::Flags::RESOLVED_NAME:
          json.value("RESOLVED_NAME");
          break;
        case FunctionFlags::Flags::RESOLVED_LENGTH:
          json.value("RESOLVED_LENGTH");
          break;
        default:
          json.value("Unknown(%x)", i);
          break;
      }
    }
  }
}

static void DumpScriptThing(js::JSONPrinter& json,
                            const CompilationStencil* stencil,
                            TaggedScriptThingIndex thing) {
  switch (thing.tag()) {
    case TaggedScriptThingIndex::Kind::ParserAtomIndex:
    case TaggedScriptThingIndex::Kind::WellKnown:
      json.beginObject();
      json.property("type", "Atom");
      DumpTaggedParserAtomIndex(json, thing.toAtom(), stencil);
      json.endObject();
      break;
    case TaggedScriptThingIndex::Kind::Null:
      json.nullValue();
      break;
    case TaggedScriptThingIndex::Kind::BigInt:
      json.value("BigIntIndex(%zu)", size_t(thing.toBigInt()));
      break;
    case TaggedScriptThingIndex::Kind::ObjLiteral:
      json.value("ObjLiteralIndex(%zu)", size_t(thing.toObjLiteral()));
      break;
    case TaggedScriptThingIndex::Kind::RegExp:
      json.value("RegExpIndex(%zu)", size_t(thing.toRegExp()));
      break;
    case TaggedScriptThingIndex::Kind::Scope:
      json.value("ScopeIndex(%zu)", size_t(thing.toScope()));
      break;
    case TaggedScriptThingIndex::Kind::Function:
      json.value("ScriptIndex(%zu)", size_t(thing.toFunction()));
      break;
    case TaggedScriptThingIndex::Kind::EmptyGlobalScope:
      json.value("EmptyGlobalScope");
      break;
  }
}

void ScriptStencil::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json, nullptr);
}

void ScriptStencil::dump(js::JSONPrinter& json,
                         const CompilationStencil* stencil) const {
  json.beginObject();
  dumpFields(json, stencil);
  json.endObject();
}

void ScriptStencil::dumpFields(js::JSONPrinter& json,
                               const CompilationStencil* stencil) const {
  json.formatProperty("gcThingsOffset", "CompilationGCThingIndex(%u)",
                      gcThingsOffset.index);
  json.property("gcThingsLength", gcThingsLength);

  if (stencil) {
    json.beginListProperty("gcThings");
    for (const auto& thing : gcthings(*stencil)) {
      DumpScriptThing(json, stencil, thing);
    }
    json.endList();
  }

  json.beginListProperty("flags");
  if (flags_ & WasEmittedByEnclosingScriptFlag) {
    json.value("WasEmittedByEnclosingScriptFlag");
  }
  if (flags_ & AllowRelazifyFlag) {
    json.value("AllowRelazifyFlag");
  }
  if (flags_ & HasSharedDataFlag) {
    json.value("HasSharedDataFlag");
  }
  if (flags_ & HasLazyFunctionEnclosingScopeIndexFlag) {
    json.value("HasLazyFunctionEnclosingScopeIndexFlag");
  }
  json.endList();

  if (isFunction()) {
    json.beginObjectProperty("functionAtom");
    DumpTaggedParserAtomIndex(json, functionAtom, stencil);
    json.endObject();

    json.beginListProperty("functionFlags");
    DumpFunctionFlagsItems(json, functionFlags);
    json.endList();

    if (hasLazyFunctionEnclosingScopeIndex()) {
      json.formatProperty("lazyFunctionEnclosingScopeIndex", "ScopeIndex(%zu)",
                          size_t(lazyFunctionEnclosingScopeIndex()));
    }

    if (hasSelfHostedCanonicalName()) {
      json.beginObjectProperty("selfHostCanonicalName");
      DumpTaggedParserAtomIndex(json, selfHostedCanonicalName(), stencil);
      json.endObject();
    }
  }
}

void ScriptStencilExtra::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json);
}

void ScriptStencilExtra::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

void ScriptStencilExtra::dumpFields(js::JSONPrinter& json) const {
  json.beginListProperty("immutableFlags");
  DumpImmutableScriptFlags(json, immutableFlags);
  json.endList();

  json.beginObjectProperty("extent");
  json.property("sourceStart", extent.sourceStart);
  json.property("sourceEnd", extent.sourceEnd);
  json.property("toStringStart", extent.toStringStart);
  json.property("toStringEnd", extent.toStringEnd);
  json.property("lineno", extent.lineno);
  json.property("column", extent.column);
  json.endObject();

  json.property("memberInitializers", memberInitializers_);

  json.property("nargs", nargs);
}

void SharedDataContainer::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json);
}

void SharedDataContainer::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

void SharedDataContainer::dumpFields(js::JSONPrinter& json) const {
  if (isEmpty()) {
    json.nullProperty("ScriptIndex(0)");
    return;
  }

  if (isSingle()) {
    json.formatProperty("ScriptIndex(0)", "u8[%zu]",
                        asSingle()->immutableDataLength());
    return;
  }

  if (isVector()) {
    auto& vec = *asVector();

    char index[64];
    for (size_t i = 0; i < vec.length(); i++) {
      SprintfLiteral(index, "ScriptIndex(%zu)", i);
      if (vec[i]) {
        json.formatProperty(index, "u8[%zu]", vec[i]->immutableDataLength());
      } else {
        json.nullProperty(index);
      }
    }
    return;
  }

  if (isMap()) {
    auto& map = *asMap();

    char index[64];
    for (auto iter = map.iter(); !iter.done(); iter.next()) {
      SprintfLiteral(index, "ScriptIndex(%u)", iter.get().key().index);
      json.formatProperty(index, "u8[%zu]",
                          iter.get().value()->immutableDataLength());
    }
    return;
  }

  MOZ_ASSERT(isBorrow());
  asBorrow()->dumpFields(json);
}

void CompilationStencil::dump() const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  dump(json);
  out.put("\n");
}

void CompilationStencil::dump(js::JSONPrinter& json) const {
  json.beginObject();
  dumpFields(json);
  json.endObject();
}

void CompilationStencil::dumpFields(js::JSONPrinter& json) const {
  char index[64];

  json.beginObjectProperty("scriptData");
  for (size_t i = 0; i < scriptData.size(); i++) {
    SprintfLiteral(index, "ScriptIndex(%zu)", i);
    json.beginObjectProperty(index);
    scriptData[i].dumpFields(json, this);
    json.endObject();
  }
  json.endObject();

  json.beginObjectProperty("scriptExtra");
  for (size_t i = 0; i < scriptExtra.size(); i++) {
    SprintfLiteral(index, "ScriptIndex(%zu)", i);
    json.beginObjectProperty(index);
    scriptExtra[i].dumpFields(json);
    json.endObject();
  }
  json.endObject();

  json.beginObjectProperty("scopeData");
  MOZ_ASSERT(scopeData.size() == scopeNames.size());
  for (size_t i = 0; i < scopeData.size(); i++) {
    SprintfLiteral(index, "ScopeIndex(%zu)", i);
    json.beginObjectProperty(index);
    scopeData[i].dumpFields(json, scopeNames[i], this);
    json.endObject();
  }
  json.endObject();

  json.beginObjectProperty("sharedData");
  sharedData.dumpFields(json);
  json.endObject();

  json.beginObjectProperty("regExpData");
  for (size_t i = 0; i < regExpData.size(); i++) {
    SprintfLiteral(index, "RegExpIndex(%zu)", i);
    json.beginObjectProperty(index);
    regExpData[i].dumpFields(json, this);
    json.endObject();
  }
  json.endObject();

  json.beginObjectProperty("bigIntData");
  for (size_t i = 0; i < bigIntData.size(); i++) {
    SprintfLiteral(index, "BigIntIndex(%zu)", i);
    GenericPrinter& out = json.beginStringProperty(index);
    bigIntData[i].dumpCharsNoQuote(out);
    json.endStringProperty();
  }
  json.endObject();

  json.beginObjectProperty("objLiteralData");
  for (size_t i = 0; i < objLiteralData.size(); i++) {
    SprintfLiteral(index, "ObjLiteralIndex(%zu)", i);
    json.beginObjectProperty(index);
    objLiteralData[i].dumpFields(json, this);
    json.endObject();
  }
  json.endObject();

  if (moduleMetadata) {
    json.beginObjectProperty("moduleMetadata");
    moduleMetadata->dumpFields(json, this);
    json.endObject();
  }

  json.beginObjectProperty("asmJS");
  if (asmJS) {
    for (auto iter = asmJS->moduleMap.iter(); !iter.done(); iter.next()) {
      SprintfLiteral(index, "ScriptIndex(%u)", iter.get().key().index);
      json.formatProperty(index, "asm.js");
    }
  }
  json.endObject();
}

void CompilationStencil::dumpAtom(TaggedParserAtomIndex index) const {
  js::Fprinter out(stderr);
  js::JSONPrinter json(out);
  json.beginObject();
  DumpTaggedParserAtomIndex(json, index, this);
  json.endObject();
}

#endif  // defined(DEBUG) || defined(JS_JITSPEW)

JSAtom* CompilationAtomCache::getExistingAtomAt(ParserAtomIndex index) const {
  return atoms_[index];
}

JSAtom* CompilationAtomCache::getExistingAtomAt(
    JSContext* cx, TaggedParserAtomIndex taggedIndex) const {
  if (taggedIndex.isParserAtomIndex()) {
    auto index = taggedIndex.toParserAtomIndex();
    return getExistingAtomAt(index);
  }

  if (taggedIndex.isWellKnownAtomId()) {
    auto index = taggedIndex.toWellKnownAtomId();
    return GetWellKnownAtom(cx, index);
  }

  if (taggedIndex.isLength1StaticParserString()) {
    auto index = taggedIndex.toLength1StaticParserString();
    return cx->staticStrings().getUnit(char16_t(index));
  }

  MOZ_ASSERT(taggedIndex.isLength2StaticParserString());
  auto index = taggedIndex.toLength2StaticParserString();
  return cx->staticStrings().getLength2FromIndex(size_t(index));
}

JSAtom* CompilationAtomCache::getAtomAt(ParserAtomIndex index) const {
  if (size_t(index) >= atoms_.length()) {
    return nullptr;
  }
  return atoms_[index];
}

bool CompilationAtomCache::hasAtomAt(ParserAtomIndex index) const {
  if (size_t(index) >= atoms_.length()) {
    return false;
  }
  return !!atoms_[index];
}

bool CompilationAtomCache::setAtomAt(JSContext* cx, ParserAtomIndex index,
                                     JSAtom* atom) {
  if (size_t(index) < atoms_.length()) {
    atoms_[index] = atom;
    return true;
  }

  if (!atoms_.resize(size_t(index) + 1)) {
    ReportOutOfMemory(cx);
    return false;
  }

  atoms_[index] = atom;
  return true;
}

bool CompilationAtomCache::allocate(JSContext* cx, size_t length) {
  MOZ_ASSERT(length >= atoms_.length());
  if (length == atoms_.length()) {
    return true;
  }

  if (!atoms_.resize(length)) {
    ReportOutOfMemory(cx);
    return false;
  }

  return true;
}

void CompilationAtomCache::stealBuffer(AtomCacheVector& atoms) {
  atoms_ = std::move(atoms);
  // Destroy elements, without unreserving.
  atoms_.clear();
}

void CompilationAtomCache::releaseBuffer(AtomCacheVector& atoms) {
  atoms = std::move(atoms_);
}

bool CompilationState::allocateGCThingsUninitialized(
    JSContext* cx, ScriptIndex scriptIndex, size_t length,
    TaggedScriptThingIndex** cursor) {
  MOZ_ASSERT(gcThingData.length() <= UINT32_MAX);

  auto gcThingsOffset = CompilationGCThingIndex(gcThingData.length());

  if (length > INDEX_LIMIT) {
    ReportAllocationOverflow(cx);
    return false;
  }
  uint32_t gcThingsLength = length;

  if (!gcThingData.growByUninitialized(length)) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  if (gcThingData.length() > UINT32_MAX) {
    ReportAllocationOverflow(cx);
    return false;
  }

  ScriptStencil& script = scriptData[scriptIndex];
  script.gcThingsOffset = gcThingsOffset;
  script.gcThingsLength = gcThingsLength;

  *cursor = gcThingData.begin() + gcThingsOffset;
  return true;
}

bool CompilationState::appendScriptStencilAndData(JSContext* cx) {
  if (!scriptData.emplaceBack()) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  if (isInitialStencil()) {
    if (!scriptExtra.emplaceBack()) {
      scriptData.popBack();
      MOZ_ASSERT(scriptData.length() == scriptExtra.length());

      js::ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

bool CompilationState::appendGCThings(
    JSContext* cx, ScriptIndex scriptIndex,
    mozilla::Span<const TaggedScriptThingIndex> things) {
  MOZ_ASSERT(gcThingData.length() <= UINT32_MAX);

  auto gcThingsOffset = CompilationGCThingIndex(gcThingData.length());

  if (things.size() > INDEX_LIMIT) {
    ReportAllocationOverflow(cx);
    return false;
  }
  uint32_t gcThingsLength = uint32_t(things.size());

  if (!gcThingData.append(things.data(), things.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }

  if (gcThingData.length() > UINT32_MAX) {
    ReportAllocationOverflow(cx);
    return false;
  }

  ScriptStencil& script = scriptData[scriptIndex];
  script.gcThingsOffset = gcThingsOffset;
  script.gcThingsLength = gcThingsLength;
  return true;
}

CompilationState::CompilationStatePosition CompilationState::getPosition() {
  return CompilationStatePosition{scriptData.length(),
                                  asmJS ? asmJS->moduleMap.count() : 0};
}

void CompilationState::rewind(
    const CompilationState::CompilationStatePosition& pos) {
  if (asmJS && asmJS->moduleMap.count() != pos.asmJSCount) {
    for (size_t i = pos.scriptDataLength; i < scriptData.length(); i++) {
      asmJS->moduleMap.remove(ScriptIndex(i));
    }
    MOZ_ASSERT(asmJS->moduleMap.count() == pos.asmJSCount);
  }
  // scriptExtra is empty for delazification.
  if (scriptExtra.length()) {
    MOZ_ASSERT(scriptExtra.length() == scriptData.length());
    scriptExtra.shrinkTo(pos.scriptDataLength);
  }
  scriptData.shrinkTo(pos.scriptDataLength);
}

void CompilationState::markGhost(
    const CompilationState::CompilationStatePosition& pos) {
  for (size_t i = pos.scriptDataLength; i < scriptData.length(); i++) {
    scriptData[i].setIsGhost();
  }
}

bool CompilationStencilMerger::buildFunctionKeyToIndex(JSContext* cx) {
  if (!functionKeyToInitialScriptIndex_.reserve(initial_->scriptExtra.length() -
                                                1)) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (size_t i = 1; i < initial_->scriptExtra.length(); i++) {
    const auto& extra = initial_->scriptExtra[i];
    auto key = CompilationStencil::toFunctionKey(extra.extent);

    // There can be multiple ScriptStencilExtra with same extent if
    // the function is parsed multiple times because of rewind for
    // arrow function, and in that case the last one's index should be used.
    // Overwrite with the last one.
    //
    // Already reserved above, but OOMTest can hit failure mode in
    // HashTable::add.
    if (!functionKeyToInitialScriptIndex_.put(key, ScriptIndex(i))) {
      ReportOutOfMemory(cx);
      return false;
    }
  }

  return true;
}

ScriptIndex CompilationStencilMerger::getInitialScriptIndexFor(
    const CompilationStencil& delazification) const {
  auto p = functionKeyToInitialScriptIndex_.lookup(delazification.functionKey);
  MOZ_ASSERT(p);
  return p->value();
}

bool CompilationStencilMerger::buildAtomIndexMap(
    JSContext* cx, const CompilationStencil& delazification,
    AtomIndexMap& atomIndexMap) {
  uint32_t atomCount = delazification.parserAtomData.size();
  if (!atomIndexMap.reserve(atomCount)) {
    ReportOutOfMemory(cx);
    return false;
  }
  for (const auto& atom : delazification.parserAtomData) {
    auto mappedIndex = initial_->parserAtoms.internExternalParserAtom(cx, atom);
    if (!mappedIndex) {
      return false;
    }
    if (atom->isUsedByStencil()) {
      initial_->parserAtoms.markUsedByStencil(mappedIndex);
    }
    atomIndexMap.infallibleAppend(mappedIndex);
  }
  return true;
}

bool CompilationStencilMerger::setInitial(
    JSContext* cx, UniquePtr<ExtensibleCompilationStencil>&& initial) {
  MOZ_ASSERT(!initial_);

  initial_ = std::move(initial);

  return buildFunctionKeyToIndex(cx);
}

template <typename GCThingIndexMapFunc, typename AtomIndexMapFunc,
          typename ScopeIndexMapFunc>
static void MergeScriptStencil(ScriptStencil& dest, const ScriptStencil& src,
                               GCThingIndexMapFunc mapGCThingIndex,
                               AtomIndexMapFunc mapAtomIndex,
                               ScopeIndexMapFunc mapScopeIndex,
                               bool isTopLevel) {
  // If this function was lazy, all inner functions should have been lazy.
  MOZ_ASSERT(!dest.hasSharedData());

  // If the inner lazy function is skipped, gcThingsLength is empty.
  if (src.gcThingsLength) {
    dest.gcThingsOffset = mapGCThingIndex(src.gcThingsOffset);
    dest.gcThingsLength = src.gcThingsLength;
  }

  if (src.functionAtom) {
    dest.functionAtom = mapAtomIndex(src.functionAtom);
  }

  if (!dest.hasLazyFunctionEnclosingScopeIndex() &&
      src.hasLazyFunctionEnclosingScopeIndex()) {
    // Both enclosing function and this function were lazy, and
    // now enclosing function is non-lazy and this function is still lazy.
    dest.setLazyFunctionEnclosingScopeIndex(
        mapScopeIndex(src.lazyFunctionEnclosingScopeIndex()));
  } else if (dest.hasLazyFunctionEnclosingScopeIndex() &&
             !src.hasLazyFunctionEnclosingScopeIndex()) {
    // The enclosing function was non-lazy and this function was lazy, and
    // now this function is non-lazy.
    dest.resetHasLazyFunctionEnclosingScopeIndexAfterStencilMerge();
  } else {
    // The enclosing function is still lazy.
    MOZ_ASSERT(!dest.hasLazyFunctionEnclosingScopeIndex());
    MOZ_ASSERT(!src.hasLazyFunctionEnclosingScopeIndex());
  }

#ifdef DEBUG
  uint16_t BASESCRIPT = uint16_t(FunctionFlags::Flags::BASESCRIPT);
  uint16_t HAS_INFERRED_NAME =
      uint16_t(FunctionFlags::Flags::HAS_INFERRED_NAME);
  uint16_t HAS_GUESSED_ATOM = uint16_t(FunctionFlags::Flags::HAS_GUESSED_ATOM);
  uint16_t acceptableDifferenceForLazy = HAS_INFERRED_NAME | HAS_GUESSED_ATOM;
  uint16_t acceptableDifferenceForNonLazy =
      BASESCRIPT | HAS_INFERRED_NAME | HAS_GUESSED_ATOM;

  MOZ_ASSERT_IF(
      isTopLevel,
      (dest.functionFlags.toRaw() | acceptableDifferenceForNonLazy) ==
          (src.functionFlags.toRaw() | acceptableDifferenceForNonLazy));

  // NOTE: Currently we don't delazify inner functions.
  MOZ_ASSERT_IF(!isTopLevel,
                (dest.functionFlags.toRaw() | acceptableDifferenceForLazy) ==
                    (src.functionFlags.toRaw() | acceptableDifferenceForLazy));
#endif  // DEBUG
  dest.functionFlags = src.functionFlags;

  // Other flags.

  if (src.wasEmittedByEnclosingScript()) {
    // NOTE: the top-level function of the delazification have
    //       src.wasEmittedByEnclosingScript() == false, and that shouldn't
    //       be copied.
    dest.setWasEmittedByEnclosingScript();
  }

  if (src.allowRelazify()) {
    dest.setAllowRelazify();
  }

  if (src.hasSharedData()) {
    dest.setHasSharedData();
  }
}

bool CompilationStencilMerger::addDelazification(
    JSContext* cx, const CompilationStencil& delazification) {
  MOZ_ASSERT(initial_);

  auto delazifiedFunctionIndex = getInitialScriptIndexFor(delazification);
  auto& destFun = initial_->scriptData[delazifiedFunctionIndex];

  if (destFun.hasSharedData()) {
    // If the function was already non-lazy, it means the following happened.
    //   1. this function is lazily parsed
    //   2. incremental encoding is started
    //   3. this function is delazified, and encoded
    //   4. incremental encoding is finished
    //   5. decoded and merged
    //   6. incremental encoding is started
    //      here, this function is encoded as non-lazy
    //   7. this function is relazified
    //   8. this function is delazified, and encoded
    //   9. incremental encoding is finished
    //  10. decoded and merged
    //
    // This shouldn't happen in wild, but can happen in testcase that uses
    // JS::DecodeScriptAndStartIncrementalEncoding at steps 5-6
    // (this may change in future).
    //
    // Encoding same function's delazification again shouldn't happen.
    return true;
  }

  // If any failure happens, the initial stencil is left in the broken state.
  // Immediately discard it.
  auto failureCase = mozilla::MakeScopeExit([&] { initial_.reset(); });

  mozilla::Maybe<ScopeIndex> functionEnclosingScope;
  if (destFun.hasLazyFunctionEnclosingScopeIndex()) {
    // lazyFunctionEnclosingScopeIndex_ can be Nothing if this is
    // top-level function.
    functionEnclosingScope =
        mozilla::Some(destFun.lazyFunctionEnclosingScopeIndex());
  }

  // A map from ParserAtomIndex in delazification to TaggedParserAtomIndex
  // in initial_.
  AtomIndexMap atomIndexMap;
  if (!buildAtomIndexMap(cx, delazification, atomIndexMap)) {
    return false;
  }
  auto mapAtomIndex = [&](TaggedParserAtomIndex index) {
    if (index.isParserAtomIndex()) {
      return atomIndexMap[index.toParserAtomIndex()];
    }

    return index;
  };

  size_t gcThingOffset = initial_->gcThingData.length();
  size_t regExpOffset = initial_->regExpData.length();
  size_t bigIntOffset = initial_->bigIntData.length();
  size_t objLiteralOffset = initial_->objLiteralData.length();
  size_t scopeOffset = initial_->scopeData.length();

  // Map delazification's ScriptIndex to initial's ScriptIndex.
  //
  // The lazy function's gcthings list stores inner function's ScriptIndex.
  // The n-th gcthing holds the ScriptIndex of the (n+1)-th script in
  // delazification.
  //
  // NOTE: Currently we don't delazify inner functions.
  auto lazyFunctionGCThingsOffset = destFun.gcThingsOffset;
  auto mapScriptIndex = [&](ScriptIndex index) {
    if (index == CompilationStencil::TopLevelIndex) {
      return delazifiedFunctionIndex;
    }

    return initial_->gcThingData[lazyFunctionGCThingsOffset + index.index - 1]
        .toFunction();
  };

  // Map other delazification's indices into initial's indices.
  auto mapGCThingIndex = [&](CompilationGCThingIndex offset) {
    return CompilationGCThingIndex(gcThingOffset + offset.index);
  };
  auto mapRegExpIndex = [&](RegExpIndex index) {
    return RegExpIndex(regExpOffset + index.index);
  };
  auto mapBigIntIndex = [&](BigIntIndex index) {
    return BigIntIndex(bigIntOffset + index.index);
  };
  auto mapObjLiteralIndex = [&](ObjLiteralIndex index) {
    return ObjLiteralIndex(objLiteralOffset + index.index);
  };
  auto mapScopeIndex = [&](ScopeIndex index) {
    return ScopeIndex(scopeOffset + index.index);
  };

  // Append gcThingData, with mapping TaggedScriptThingIndex.
  if (!initial_->gcThingData.append(delazification.gcThingData.data(),
                                    delazification.gcThingData.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = gcThingOffset; i < initial_->gcThingData.length(); i++) {
    auto& index = initial_->gcThingData[i];
    if (index.isNull()) {
      // Nothing to do.
    } else if (index.isAtom()) {
      index = TaggedScriptThingIndex(mapAtomIndex(index.toAtom()));
    } else if (index.isBigInt()) {
      index = TaggedScriptThingIndex(mapBigIntIndex(index.toBigInt()));
    } else if (index.isObjLiteral()) {
      index = TaggedScriptThingIndex(mapObjLiteralIndex(index.toObjLiteral()));
    } else if (index.isRegExp()) {
      index = TaggedScriptThingIndex(mapRegExpIndex(index.toRegExp()));
    } else if (index.isScope()) {
      index = TaggedScriptThingIndex(mapScopeIndex(index.toScope()));
    } else if (index.isFunction()) {
      index = TaggedScriptThingIndex(mapScriptIndex(index.toFunction()));
    } else {
      MOZ_ASSERT(index.isEmptyGlobalScope());
      // Nothing to do
    }
  }

  // Append regExpData, with mapping RegExpStencil.atom_.
  if (!initial_->regExpData.append(delazification.regExpData.data(),
                                   delazification.regExpData.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = regExpOffset; i < initial_->regExpData.length(); i++) {
    auto& data = initial_->regExpData[i];
    data.atom_ = mapAtomIndex(data.atom_);
  }

  // Append bigIntData, with copying BigIntStencil.source_.
  if (!initial_->bigIntData.reserve(bigIntOffset +
                                    delazification.bigIntData.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  for (const auto& data : delazification.bigIntData) {
    initial_->bigIntData.infallibleEmplaceBack();
    if (!initial_->bigIntData.back().init(cx, initial_->alloc, data.source())) {
      return false;
    }
  }

  // Append objLiteralData, with copying ObjLiteralStencil.code_, and mapping
  // TaggedParserAtomIndex in it.
  if (!initial_->objLiteralData.reserve(objLiteralOffset +
                                        delazification.objLiteralData.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  for (const auto& data : delazification.objLiteralData) {
    size_t length = data.code().size();
    auto* code = initial_->alloc.newArrayUninitialized<uint8_t>(length);
    if (!code) {
      js::ReportOutOfMemory(cx);
      return false;
    }
    memcpy(code, data.code().data(), length);

    ObjLiteralModifier modifier(mozilla::Span(code, length));
    modifier.mapAtom(mapAtomIndex);

    initial_->objLiteralData.infallibleEmplaceBack(code, length, data.flags(),
                                                   data.propertyCount());
  }

  // Append scopeData, with mapping indices in ScopeStencil fields.
  // And append scopeNames, with copying the entire data, and mapping
  // trailingNames.
  if (!initial_->scopeData.reserve(scopeOffset +
                                   delazification.scopeData.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  if (!initial_->scopeNames.reserve(scopeOffset +
                                    delazification.scopeNames.size())) {
    js::ReportOutOfMemory(cx);
    return false;
  }
  for (size_t i = 0; i < delazification.scopeData.size(); i++) {
    const auto& srcData = delazification.scopeData[i];
    const auto* srcNames = delazification.scopeNames[i];

    mozilla::Maybe<ScriptIndex> functionIndex = mozilla::Nothing();
    if (srcData.isFunction()) {
      // Inner functions should be in the same order as initial, beginning from
      // the delazification's index.
      functionIndex = mozilla::Some(mapScriptIndex(srcData.functionIndex()));
    }

    BaseParserScopeData* destNames = nullptr;
    if (srcNames) {
      destNames = CopyScopeData(cx, initial_->alloc, srcData.kind(), srcNames);
      if (!destNames) {
        return false;
      }
      auto trailingNames =
          GetParserScopeDataTrailingNames(srcData.kind(), destNames);
      for (auto& name : trailingNames) {
        if (name.name()) {
          name.updateNameAfterStencilMerge(mapAtomIndex(name.name()));
        }
      }
    }

    initial_->scopeData.infallibleEmplaceBack(
        srcData.kind(),
        srcData.hasEnclosing()
            ? mozilla::Some(mapScopeIndex(srcData.enclosing()))
            : functionEnclosingScope,
        srcData.firstFrameSlot(),
        srcData.hasEnvironmentShape()
            ? mozilla::Some(srcData.numEnvironmentSlots())
            : mozilla::Nothing(),
        functionIndex, srcData.isArrow());

    initial_->scopeNames.infallibleEmplaceBack(destNames);
  }

  // Add delazified function's shared data.
  //
  // NOTE: Currently we don't delazify inner functions.
  if (!initial_->sharedData.addExtraWithoutShare(
          cx, delazifiedFunctionIndex,
          delazification.sharedData.get(CompilationStencil::TopLevelIndex))) {
    return false;
  }

  // Update scriptData, with mapping indices in ScriptStencil fields.
  for (uint32_t i = 0; i < delazification.scriptData.size(); i++) {
    auto destIndex = mapScriptIndex(ScriptIndex(i));
    MergeScriptStencil(initial_->scriptData[destIndex],
                       delazification.scriptData[i], mapGCThingIndex,
                       mapAtomIndex, mapScopeIndex,
                       i == CompilationStencil::TopLevelIndex);
  }

  // Function shouldn't be a module.
  MOZ_ASSERT(!delazification.moduleMetadata);

  // asm.js shouldn't appear inside delazification, given asm.js forces
  // full-parse.
  MOZ_ASSERT(!delazification.asmJS);

  failureCase.release();
  return true;
}

void JS::StencilAddRef(JS::Stencil* stencil) { stencil->refCount++; }
void JS::StencilRelease(JS::Stencil* stencil) {
  MOZ_RELEASE_ASSERT(stencil->refCount > 0);
  stencil->refCount--;
  if (stencil->refCount == 0) {
    js_delete(stencil);
  }
}

template <typename CharT>
static already_AddRefed<JS::Stencil> CompileGlobalScriptToStencilImpl(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<CharT>& srcBuf) {
  ScopeKind scopeKind =
      options.nonSyntacticScope ? ScopeKind::NonSyntactic : ScopeKind::Global;

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  auto stencil = js::frontend::CompileGlobalScriptToStencil(cx, input.get(),
                                                            srcBuf, scopeKind);
  if (!stencil) {
    return nullptr;
  }

  // Convert the UniquePtr to a RefPtr and increment the count (to 1).
  return do_AddRef(stencil.release());
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileGlobalScriptToStencilImpl(cx, options, srcBuf);
}

already_AddRefed<JS::Stencil> JS::CompileGlobalScriptToStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf) {
  return CompileGlobalScriptToStencilImpl(cx, options, srcBuf);
}

template <typename CharT>
static already_AddRefed<JS::Stencil> CompileModuleScriptToStencilImpl(
    JSContext* cx, const JS::ReadOnlyCompileOptions& optionsInput,
    JS::SourceText<CharT>& srcBuf) {
  JS::CompileOptions options(cx, optionsInput);
  options.setModule();

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  auto stencil = js::frontend::ParseModuleToStencil(cx, input.get(), srcBuf);
  if (!stencil) {
    return nullptr;
  }

  // Convert the UniquePtr to a RefPtr and increment the count (to 1).
  return do_AddRef(stencil.release());
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<mozilla::Utf8Unit>& srcBuf) {
  return CompileModuleScriptToStencilImpl(cx, options, srcBuf);
}

already_AddRefed<JS::Stencil> JS::CompileModuleScriptToStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    JS::SourceText<char16_t>& srcBuf) {
  return CompileModuleScriptToStencilImpl(cx, options, srcBuf);
}

JSScript* JS::InstantiateGlobalStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    RefPtr<JS::Stencil> stencil) {
  if (stencil->canLazilyParse != CanLazilyParse(options)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_STENCIL_OPTIONS_MISMATCH);
    return nullptr;
  }

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  Rooted<CompilationGCOutput> gcOutput(cx);
  if (!InstantiateStencils(cx, input.get(), *stencil, gcOutput.get())) {
    return nullptr;
  }

  return gcOutput.get().script;
}

JSObject* JS::InstantiateModuleStencil(
    JSContext* cx, const JS::ReadOnlyCompileOptions& optionsInput,
    RefPtr<JS::Stencil> stencil) {
  JS::CompileOptions options(cx, optionsInput);
  options.setModule();

  if (stencil->canLazilyParse != CanLazilyParse(options)) {
    JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                              JSMSG_STENCIL_OPTIONS_MISMATCH);
    return nullptr;
  }

  Rooted<CompilationInput> input(cx, CompilationInput(options));
  Rooted<CompilationGCOutput> gcOutput(cx);
  if (!InstantiateStencils(cx, input.get(), *stencil, gcOutput.get())) {
    return nullptr;
  }

  return gcOutput.get().module;
}

JS::TranscodeResult JS::EncodeStencil(JSContext* cx,
                                      const JS::ReadOnlyCompileOptions& options,
                                      RefPtr<JS::Stencil> stencil,
                                      TranscodeBuffer& buffer) {
  Rooted<CompilationInput> input(cx, CompilationInput(options));
  XDRStencilEncoder encoder(cx, buffer);
  XDRResult res = encoder.codeStencil(input.get(), *stencil);
  if (res.isErr()) {
    return res.unwrapErr();
  }
  return TranscodeResult::Ok;
}

JS::TranscodeResult JS::DecodeStencil(JSContext* cx,
                                      const JS::ReadOnlyCompileOptions& options,
                                      const JS::TranscodeRange& range,
                                      RefPtr<JS::Stencil>& stencilOut) {
  Rooted<CompilationInput> input(cx, CompilationInput(options));
  if (!input.get().initForGlobal(cx)) {
    return TranscodeResult::Throw;
  }
  UniquePtr<JS::Stencil> stencil(
      MakeUnique<CompilationStencil>(input.get().source));
  if (!stencil) {
    return TranscodeResult::Throw;
  }
  XDRStencilDecoder decoder(cx, range);
  XDRResult res = decoder.codeStencil(input.get(), *stencil);
  if (res.isErr()) {
    return res.unwrapErr();
  }
  stencilOut = do_AddRef(stencil.release());
  return TranscodeResult::Ok;
}

already_AddRefed<JS::Stencil> JS::FinishOffThreadStencil(
    JSContext* cx, JS::OffThreadToken* token) {
  MOZ_ASSERT(cx);
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  return do_AddRef(HelperThreadState().finishStencilParseTask(cx, token));
}
