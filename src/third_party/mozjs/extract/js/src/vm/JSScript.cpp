/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS script operations.
 */

#include "vm/JSScript-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Span.h"  // mozilla::{Span,Span}
#include "mozilla/Sprintf.h"
#include "mozilla/Utf8.h"
#include "mozilla/Vector.h"

#include <algorithm>
#include <new>
#include <string.h>
#include <type_traits>
#include <utility>

#include "jsapi.h"
#include "jstypes.h"

#include "frontend/BytecodeCompiler.h"
#include "frontend/BytecodeEmitter.h"
#include "frontend/CompilationStencil.h"  // frontend::CompilationStencil
#include "frontend/SharedContext.h"
#include "frontend/SourceNotes.h"  // SrcNote, SrcNoteType, SrcNoteIterator
#include "frontend/StencilXdr.h"  // frontend::StencilXdr::SharedData, CanCopyDataToDisk
#include "gc/FreeOp.h"
#include "jit/BaselineJIT.h"
#include "jit/CacheIRHealth.h"
#include "jit/Invalidation.h"
#include "jit/Ion.h"
#include "jit/IonScript.h"
#include "jit/JitCode.h"
#include "jit/JitOptions.h"
#include "jit/JitRuntime.h"
#include "js/CompileOptions.h"
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/MemoryMetrics.h"
#include "js/Printf.h"
#include "js/SourceText.h"
#include "js/Transcoding.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Wrapper.h"
#include "util/Memory.h"
#include "util/Poison.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/ArgumentsObject.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"
#include "vm/BytecodeUtil.h"
#include "vm/Compression.h"
#include "vm/FunctionFlags.h"      // js::FunctionFlags
#include "vm/HelperThreadState.h"  // js::RunPendingSourceCompressions
#include "vm/JSAtom.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSObject.h"
#include "vm/Opcodes.h"
#include "vm/PlainObject.h"  // js::PlainObject
#include "vm/SelfHosting.h"
#include "vm/Shape.h"
#include "vm/SharedImmutableStringsCache.h"
#include "vm/Warnings.h"  // js::WarnNumberLatin1
#include "vm/Xdr.h"
#ifdef MOZ_VTUNE
#  include "vtune/VTuneWrapper.h"
#endif

#include "debugger/DebugAPI-inl.h"
#include "gc/Marking-inl.h"
#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/JSFunction-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/NativeObject-inl.h"
#include "vm/SharedImmutableStringsCache-inl.h"
#include "vm/Stack-inl.h"

using namespace js;

using mozilla::CheckedInt;
using mozilla::Maybe;
using mozilla::PodCopy;
using mozilla::PointerRangeSize;
using mozilla::Utf8AsUnsignedChars;
using mozilla::Utf8Unit;

using JS::CompileOptions;
using JS::ReadOnlyCompileOptions;
using JS::SourceText;

template <XDRMode mode>
XDRResult js::XDRScriptConst(XDRState<mode>* xdr, MutableHandleValue vp) {
  JSContext* cx = xdr->cx();

  enum ConstTag {
    SCRIPT_INT,
    SCRIPT_DOUBLE,
    SCRIPT_ATOM,
    SCRIPT_TRUE,
    SCRIPT_FALSE,
    SCRIPT_NULL,
    SCRIPT_OBJECT,
    SCRIPT_VOID,
    SCRIPT_HOLE,
    SCRIPT_BIGINT
  };

  ConstTag tag;
  if (mode == XDR_ENCODE) {
    if (vp.isInt32()) {
      tag = SCRIPT_INT;
    } else if (vp.isDouble()) {
      tag = SCRIPT_DOUBLE;
    } else if (vp.isString()) {
      tag = SCRIPT_ATOM;
    } else if (vp.isTrue()) {
      tag = SCRIPT_TRUE;
    } else if (vp.isFalse()) {
      tag = SCRIPT_FALSE;
    } else if (vp.isNull()) {
      tag = SCRIPT_NULL;
    } else if (vp.isObject()) {
      tag = SCRIPT_OBJECT;
    } else if (vp.isMagic(JS_ELEMENTS_HOLE)) {
      tag = SCRIPT_HOLE;
    } else if (vp.isBigInt()) {
      tag = SCRIPT_BIGINT;
    } else {
      MOZ_ASSERT(vp.isUndefined());
      tag = SCRIPT_VOID;
    }
  }

  MOZ_TRY(xdr->codeEnum32(&tag));

  switch (tag) {
    case SCRIPT_INT: {
      uint32_t i;
      if (mode == XDR_ENCODE) {
        i = uint32_t(vp.toInt32());
      }
      MOZ_TRY(xdr->codeUint32(&i));
      if (mode == XDR_DECODE) {
        vp.set(Int32Value(int32_t(i)));
      }
      break;
    }
    case SCRIPT_DOUBLE: {
      double d;
      if (mode == XDR_ENCODE) {
        d = vp.toDouble();
      }
      MOZ_TRY(xdr->codeDouble(&d));
      if (mode == XDR_DECODE) {
        vp.set(DoubleValue(d));
      }
      break;
    }
    case SCRIPT_ATOM: {
      RootedAtom atom(cx);
      if (mode == XDR_ENCODE) {
        atom = &vp.toString()->asAtom();
      }
      MOZ_TRY(XDRAtom(xdr, &atom));
      if (mode == XDR_DECODE) {
        vp.set(StringValue(atom));
      }
      break;
    }
    case SCRIPT_TRUE:
      if (mode == XDR_DECODE) {
        vp.set(BooleanValue(true));
      }
      break;
    case SCRIPT_FALSE:
      if (mode == XDR_DECODE) {
        vp.set(BooleanValue(false));
      }
      break;
    case SCRIPT_NULL:
      if (mode == XDR_DECODE) {
        vp.set(NullValue());
      }
      break;
    case SCRIPT_OBJECT: {
      RootedObject obj(cx);
      if (mode == XDR_ENCODE) {
        obj = &vp.toObject();
      }

      MOZ_TRY(XDRObjectLiteral(xdr, &obj));

      if (mode == XDR_DECODE) {
        vp.setObject(*obj);
      }
      break;
    }
    case SCRIPT_VOID:
      if (mode == XDR_DECODE) {
        vp.set(UndefinedValue());
      }
      break;
    case SCRIPT_HOLE:
      if (mode == XDR_DECODE) {
        vp.setMagic(JS_ELEMENTS_HOLE);
      }
      break;
    case SCRIPT_BIGINT: {
      RootedBigInt bi(cx);
      if (mode == XDR_ENCODE) {
        bi = vp.toBigInt();
      }

      MOZ_TRY(XDRBigInt(xdr, &bi));

      if (mode == XDR_DECODE) {
        vp.setBigInt(bi);
      }
      break;
    }
    default:
      // Fail in debug, but only soft-fail in release
      MOZ_ASSERT(false, "Bad XDR value kind");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
  }
  return Ok();
}

template XDRResult js::XDRScriptConst(XDRState<XDR_ENCODE>*,
                                      MutableHandleValue);

template XDRResult js::XDRScriptConst(XDRState<XDR_DECODE>*,
                                      MutableHandleValue);

// Code lazy scripts's closed over bindings.
template <XDRMode mode>
/* static */
XDRResult BaseScript::XDRLazyScriptData(XDRState<mode>* xdr,
                                        HandleScriptSourceObject sourceObject,
                                        Handle<BaseScript*> lazy) {
  JSContext* cx = xdr->cx();

  RootedAtom atom(cx);
  RootedFunction func(cx);

  if (lazy->useMemberInitializers()) {
    uint32_t bits;
    if (mode == XDR_ENCODE) {
      MOZ_ASSERT(lazy->getMemberInitializers().valid);
      bits = lazy->getMemberInitializers().serialize();
    }
    MOZ_TRY(xdr->codeUint32(&bits));
    if (mode == XDR_DECODE) {
      lazy->setMemberInitializers(MemberInitializers::deserialize(bits));
    }
  }

  mozilla::Span<JS::GCCellPtr> gcThings =
      lazy->data_ ? lazy->data_->gcthings() : mozilla::Span<JS::GCCellPtr>();

  for (JS::GCCellPtr& elem : gcThings) {
    JS::TraceKind kind = elem.kind();

    MOZ_TRY(xdr->codeEnum32(&kind));

    switch (kind) {
      case JS::TraceKind::Object: {
        if (mode == XDR_ENCODE) {
          func = &elem.as<JSObject>().as<JSFunction>();
        }
        MOZ_TRY(XDRInterpretedFunction(xdr, nullptr, sourceObject, &func));
        if (mode == XDR_DECODE) {
          func->setEnclosingLazyScript(lazy);

          elem = JS::GCCellPtr(func);
        }
        break;
      }

      case JS::TraceKind::String: {
        if (mode == XDR_ENCODE) {
          gc::Cell* cell = elem.asCell();
          MOZ_ASSERT_IF(cell, cell->as<JSString>()->isAtom());
          atom = static_cast<JSAtom*>(cell);
        }
        MOZ_TRY(XDRAtom(xdr, &atom));
        if (mode == XDR_DECODE) {
          elem = JS::GCCellPtr(static_cast<JSString*>(atom));
        }
        break;
      }

      case JS::TraceKind::Null: {
        // This is default so nothing to do.
        MOZ_ASSERT(!elem);
        break;
      }

      default: {
        // Fail in debug, but only soft-fail in release
        MOZ_ASSERT(false, "Bad XDR class kind");
        return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
      }
    }
  }

  return Ok();
}

static inline uint32_t FindScopeIndex(mozilla::Span<const JS::GCCellPtr> scopes,
                                      Scope& scope) {
  unsigned length = scopes.size();
  for (uint32_t i = 0; i < length; ++i) {
    if (scopes[i].asCell() == &scope) {
      return i;
    }
  }

  MOZ_CRASH("Scope not found");
}

template <XDRMode mode>
static XDRResult XDRInnerObject(XDRState<mode>* xdr,
                                js::PrivateScriptData* data,
                                HandleScriptSourceObject sourceObject,
                                MutableHandleObject inner) {
  enum class ClassKind { RegexpObject, JSFunction, JSObject, ArrayObject };
  JSContext* cx = xdr->cx();

  ClassKind classk;

  if (mode == XDR_ENCODE) {
    if (inner->is<RegExpObject>()) {
      classk = ClassKind::RegexpObject;
    } else if (inner->is<JSFunction>()) {
      classk = ClassKind::JSFunction;
    } else if (inner->is<PlainObject>()) {
      classk = ClassKind::JSObject;
    } else if (inner->is<ArrayObject>()) {
      classk = ClassKind::ArrayObject;
    } else {
      MOZ_CRASH("Cannot encode this class of object.");
    }
  }

  MOZ_TRY(xdr->codeEnum32(&classk));

  switch (classk) {
    case ClassKind::RegexpObject: {
      Rooted<RegExpObject*> regexp(cx);
      if (mode == XDR_ENCODE) {
        regexp = &inner->as<RegExpObject>();
      }
      MOZ_TRY(XDRScriptRegExpObject(xdr, &regexp));
      if (mode == XDR_DECODE) {
        inner.set(regexp);
      }
      break;
    }

    case ClassKind::JSFunction: {
      /* Code the nested function's enclosing scope. */
      uint32_t funEnclosingScopeIndex = 0;
      RootedScope funEnclosingScope(cx);

      if (mode == XDR_ENCODE) {
        RootedFunction function(cx, &inner->as<JSFunction>());

        if (function->isAsmJSNative()) {
          return xdr->fail(JS::TranscodeResult::Failure_AsmJSNotSupported);
        }

        MOZ_ASSERT(function->enclosingScope());
        funEnclosingScope = function->enclosingScope();
        funEnclosingScopeIndex =
            FindScopeIndex(data->gcthings(), *funEnclosingScope);
      }

      MOZ_TRY(xdr->codeUint32(&funEnclosingScopeIndex));

      if (mode == XDR_DECODE) {
        funEnclosingScope =
            &data->gcthings()[funEnclosingScopeIndex].as<Scope>();
      }

      // Code nested function and script.
      RootedFunction tmp(cx);
      if (mode == XDR_ENCODE) {
        tmp = &inner->as<JSFunction>();
      }
      MOZ_TRY(
          XDRInterpretedFunction(xdr, funEnclosingScope, sourceObject, &tmp));
      if (mode == XDR_DECODE) {
        inner.set(tmp);
      }
      break;
    }

    case ClassKind::JSObject:
    case ClassKind::ArrayObject: {
      /* Code object literal. */
      RootedObject tmp(cx);
      if (mode == XDR_ENCODE) {
        tmp = inner.get();
      }
      MOZ_TRY(XDRObjectLiteral(xdr, &tmp));
      if (mode == XDR_DECODE) {
        inner.set(tmp);
      }
      break;
    }

    default: {
      // Fail in debug, but only soft-fail in release
      MOZ_ASSERT(false, "Bad XDR class kind");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
    }
  }

  return Ok();
}

template <XDRMode mode>
static XDRResult XDRScope(XDRState<mode>* xdr, js::PrivateScriptData* data,
                          HandleScope scriptEnclosingScope,
                          HandleObject funOrMod, bool isFirstScope,
                          MutableHandleScope scope) {
  JSContext* cx = xdr->cx();

  ScopeKind scopeKind;
  RootedScope enclosing(cx);
  RootedFunction fun(cx);
  RootedModuleObject module(cx);
  uint32_t enclosingIndex = 0;

  // The enclosingScope is encoded using an integer index into the scope array.
  // This means that scopes must be topologically sorted.
  if (mode == XDR_ENCODE) {
    scopeKind = scope->kind();

    if (isFirstScope) {
      enclosingIndex = UINT32_MAX;
    } else {
      MOZ_ASSERT(scope->enclosing());
      enclosingIndex = FindScopeIndex(data->gcthings(), *scope->enclosing());
    }
  }

  MOZ_TRY(xdr->codeEnum32(&scopeKind));
  MOZ_TRY(xdr->codeUint32(&enclosingIndex));

  if (mode == XDR_DECODE) {
    if (isFirstScope) {
      MOZ_ASSERT(enclosingIndex == UINT32_MAX);
      enclosing = scriptEnclosingScope;
    } else {
      enclosing = &data->gcthings()[enclosingIndex].as<Scope>();
    }

    if (funOrMod && funOrMod->is<ModuleObject>()) {
      module.set(funOrMod.as<ModuleObject>());
    } else if (funOrMod && funOrMod->is<JSFunction>()) {
      fun.set(funOrMod.as<JSFunction>());
    }
  }

  switch (scopeKind) {
    case ScopeKind::Function:
      MOZ_TRY(FunctionScope::XDR(xdr, fun, enclosing, scope));
      break;
    case ScopeKind::FunctionBodyVar:
      MOZ_TRY(VarScope::XDR(xdr, scopeKind, enclosing, scope));
      break;
    case ScopeKind::Lexical:
    case ScopeKind::SimpleCatch:
    case ScopeKind::Catch:
    case ScopeKind::NamedLambda:
    case ScopeKind::StrictNamedLambda:
    case ScopeKind::FunctionLexical:
      MOZ_TRY(LexicalScope::XDR(xdr, scopeKind, enclosing, scope));
      break;
    case ScopeKind::ClassBody:
      MOZ_TRY(ClassBodyScope::XDR(xdr, scopeKind, enclosing, scope));
      break;
    case ScopeKind::With:
      MOZ_TRY(WithScope::XDR(xdr, enclosing, scope));
      break;
    case ScopeKind::Eval:
    case ScopeKind::StrictEval:
      MOZ_TRY(EvalScope::XDR(xdr, scopeKind, enclosing, scope));
      break;
    case ScopeKind::Global:
    case ScopeKind::NonSyntactic:
      MOZ_TRY(GlobalScope::XDR(xdr, scopeKind, scope));
      break;
    case ScopeKind::Module:
      MOZ_TRY(ModuleScope::XDR(xdr, module, enclosing, scope));
      break;
    case ScopeKind::WasmInstance:
      MOZ_CRASH("NYI");
      break;
    case ScopeKind::WasmFunction:
      MOZ_CRASH("wasm functions cannot be nested in JSScripts");
      break;
    default:
      // Fail in debug, but only soft-fail in release
      MOZ_ASSERT(false, "Bad XDR scope kind");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
  }

  return Ok();
}

template <XDRMode mode>
static XDRResult XDRScriptGCThing(XDRState<mode>* xdr, PrivateScriptData* data,
                                  HandleScriptSourceObject sourceObject,
                                  HandleScope scriptEnclosingScope,
                                  HandleObject funOrMod, bool* isFirstScope,
                                  JS::GCCellPtr* thingp) {
  JSContext* cx = xdr->cx();

  JS::TraceKind kind = thingp->kind();

  MOZ_TRY(xdr->codeEnum32(&kind));

  switch (kind) {
    case JS::TraceKind::String: {
      RootedAtom atom(cx);
      if (mode == XDR_ENCODE) {
        atom = &thingp->as<JSString>().asAtom();
      }
      MOZ_TRY(XDRAtom(xdr, &atom));
      if (mode == XDR_DECODE) {
        *thingp = JS::GCCellPtr(atom.get());
      }
      break;
    }

    case JS::TraceKind::Object: {
      RootedObject obj(cx);
      if (mode == XDR_ENCODE) {
        obj = &thingp->as<JSObject>();
      }
      MOZ_TRY(XDRInnerObject(xdr, data, sourceObject, &obj));
      if (mode == XDR_DECODE) {
        *thingp = JS::GCCellPtr(obj.get());
      }
      break;
    }

    case JS::TraceKind::Scope: {
      RootedScope scope(cx);
      if (mode == XDR_ENCODE) {
        scope = &thingp->as<Scope>();
      }
      MOZ_TRY(XDRScope(xdr, data, scriptEnclosingScope, funOrMod, *isFirstScope,
                       &scope));
      if (mode == XDR_DECODE) {
        *thingp = JS::GCCellPtr(scope.get());
      }
      *isFirstScope = false;
      break;
    }

    case JS::TraceKind::BigInt: {
      RootedBigInt bi(cx);
      if (mode == XDR_ENCODE) {
        bi = &thingp->as<BigInt>();
      }
      MOZ_TRY(XDRBigInt(xdr, &bi));
      if (mode == XDR_DECODE) {
        *thingp = JS::GCCellPtr(bi.get());
      }
      break;
    }

    default:
      // Fail in debug, but only soft-fail in release.
      MOZ_ASSERT(false, "Bad XDR class kind");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
  }
  return Ok();
}

bool js::BaseScript::isUsingInterpreterTrampoline(JSRuntime* rt) const {
  return jitCodeRaw() == rt->jitRuntime()->interpreterStub().value;
}

js::ScriptSource* js::BaseScript::maybeForwardedScriptSource() const {
  return MaybeForwarded(sourceObject())->source();
}

void js::BaseScript::setEnclosingScript(BaseScript* enclosingScript) {
  MOZ_ASSERT(enclosingScript);
  warmUpData_.initEnclosingScript(enclosingScript);
}

void js::BaseScript::setEnclosingScope(Scope* enclosingScope) {
  if (warmUpData_.isEnclosingScript()) {
    warmUpData_.clearEnclosingScript();
  }

  MOZ_ASSERT(enclosingScope);
  warmUpData_.initEnclosingScope(enclosingScope);
}

void js::BaseScript::finalize(JSFreeOp* fop) {
  // Scripts with bytecode may have optional data stored in per-runtime or
  // per-zone maps. Note that a failed compilation must not have entries since
  // the script itself will not be marked as having bytecode.
  if (hasBytecode()) {
    JSScript* script = this->asJSScript();

    if (coverage::IsLCovEnabled()) {
      coverage::CollectScriptCoverage(script, true);
    }

    script->destroyScriptCounts();
  }

  fop->runtime()->geckoProfiler().onScriptFinalized(this);

#ifdef MOZ_VTUNE
  if (zone()->scriptVTuneIdMap) {
    // Note: we should only get here if the VTune JIT profiler is running.
    zone()->scriptVTuneIdMap->remove(this);
  }
#endif

  if (warmUpData_.isJitScript()) {
    JSScript* script = this->asJSScript();
#ifdef JS_CACHEIR_SPEW
    maybeUpdateWarmUpCount(script);
#endif
    script->releaseJitScriptOnFinalize(fop);
  }

#ifdef JS_CACHEIR_SPEW
  if (hasBytecode()) {
    maybeSpewScriptFinalWarmUpCount(this->asJSScript());
  }
#endif

  if (data_) {
    // We don't need to triger any barriers here, just free the memory.
    size_t size = data_->allocationSize();
    AlwaysPoison(data_, JS_POISONED_JSSCRIPT_DATA_PATTERN, size,
                 MemCheckKind::MakeNoAccess);
    fop->free_(this, data_, size, MemoryUse::ScriptPrivateData);
  }

  freeSharedData();
}

js::Scope* js::BaseScript::releaseEnclosingScope() {
  Scope* enclosing = warmUpData_.toEnclosingScope();
  warmUpData_.clearEnclosingScope();
  return enclosing;
}

void js::BaseScript::swapData(UniquePtr<PrivateScriptData>& other) {
  PrivateScriptData* tmp = other.release();

  if (data_) {
    // When disconnecting script data from the BaseScript, we must pre-barrier
    // all edges contained in it. Those edges are no longer reachable from
    // current location in the graph.
    PreWriteBarrier(zone(), data_);

    RemoveCellMemory(this, data_->allocationSize(),
                     MemoryUse::ScriptPrivateData);
  }

  std::swap(tmp, data_);

  if (data_) {
    AddCellMemory(this, data_->allocationSize(), MemoryUse::ScriptPrivateData);
  }

  other.reset(tmp);
}

js::Scope* js::BaseScript::enclosingScope() const {
  MOZ_ASSERT(!warmUpData_.isEnclosingScript(),
             "Enclosing scope is not computed yet");

  if (warmUpData_.isEnclosingScope()) {
    return warmUpData_.toEnclosingScope();
  }

  MOZ_ASSERT(data_, "Script doesn't seem to be compiled");

  return gcthings()[js::GCThingIndex::outermostScopeIndex()]
      .as<Scope>()
      .enclosing();
}

size_t JSScript::numAlwaysLiveFixedSlots() const {
  if (bodyScope()->is<js::FunctionScope>()) {
    return bodyScope()->as<js::FunctionScope>().nextFrameSlot();
  }
  if (bodyScope()->is<js::ModuleScope>()) {
    return bodyScope()->as<js::ModuleScope>().nextFrameSlot();
  }
  if (bodyScope()->is<js::EvalScope>() &&
      bodyScope()->kind() == ScopeKind::StrictEval) {
    return bodyScope()->as<js::EvalScope>().nextFrameSlot();
  }
  return 0;
}

unsigned JSScript::numArgs() const {
  if (bodyScope()->is<js::FunctionScope>()) {
    return bodyScope()->as<js::FunctionScope>().numPositionalFormalParameters();
  }
  return 0;
}

bool JSScript::functionHasParameterExprs() const {
  // Only functions have parameters.
  js::Scope* scope = bodyScope();
  if (!scope->is<js::FunctionScope>()) {
    return false;
  }
  return scope->as<js::FunctionScope>().hasParameterExprs();
}

js::ModuleObject* JSScript::module() const {
  if (bodyScope()->is<js::ModuleScope>()) {
    return bodyScope()->as<js::ModuleScope>().module();
  }
  return nullptr;
}

bool JSScript::isGlobalCode() const {
  return bodyScope()->is<js::GlobalScope>();
}

js::VarScope* JSScript::functionExtraBodyVarScope() const {
  MOZ_ASSERT(functionHasExtraBodyVarScope());
  for (JS::GCCellPtr gcThing : gcthings()) {
    if (!gcThing.is<js::Scope>()) {
      continue;
    }
    js::Scope* scope = &gcThing.as<js::Scope>();
    if (scope->kind() == js::ScopeKind::FunctionBodyVar) {
      return &scope->as<js::VarScope>();
    }
  }
  MOZ_CRASH("Function extra body var scope not found");
}

bool JSScript::needsBodyEnvironment() const {
  for (JS::GCCellPtr gcThing : gcthings()) {
    if (!gcThing.is<js::Scope>()) {
      continue;
    }
    js::Scope* scope = &gcThing.as<js::Scope>();
    if (ScopeKindIsInBody(scope->kind()) && scope->hasEnvironment()) {
      return true;
    }
  }
  return false;
}

bool JSScript::isDirectEvalInFunction() const {
  if (!isForEval()) {
    return false;
  }
  return bodyScope()->hasOnChain(js::ScopeKind::Function);
}

template <XDRMode mode>
/* static */
XDRResult js::PrivateScriptData::XDR(XDRState<mode>* xdr, HandleScript script,
                                     HandleScriptSourceObject sourceObject,
                                     HandleScope scriptEnclosingScope,
                                     HandleObject funOrMod) {
  uint32_t ngcthings = 0;

  JSContext* cx = xdr->cx();
  PrivateScriptData* data = nullptr;

  if (mode == XDR_ENCODE) {
    data = script->data_;

    ngcthings = data->gcthings().size();
  }

  MOZ_TRY(xdr->codeUint32(&ngcthings));

  if (mode == XDR_DECODE) {
    if (!JSScript::createPrivateScriptData(cx, script, ngcthings)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    data = script->data_;
  }

  // Code the field initializer data.
  if (script->useMemberInitializers()) {
    uint32_t bits;
    if (mode == XDR_ENCODE) {
      MOZ_ASSERT(data->getMemberInitializers().valid);
      bits = data->getMemberInitializers().serialize();
    }
    MOZ_TRY(xdr->codeUint32(&bits));
    if (mode == XDR_DECODE) {
      data->setMemberInitializers(MemberInitializers::deserialize(bits));
    }
  }

  bool isFirstScope = true;
  for (JS::GCCellPtr& gcThing : data->gcthings()) {
    MOZ_TRY(XDRScriptGCThing(xdr, data, sourceObject, scriptEnclosingScope,
                             funOrMod, &isFirstScope, &gcThing));
  }

  // Verify marker to detect data corruption after decoding GC things. A
  // mismatch here indicates we will almost certainly crash in release.
  MOZ_TRY(xdr->codeMarker(0xF83B989A));

  return Ok();
}

// Initialize the optional arrays in the trailing allocation. This is a set of
// offsets that delimit each optional array followed by the arrays themselves.
// See comment before 'ImmutableScriptData' for more details.
void ImmutableScriptData::initOptionalArrays(Offset* pcursor,
                                             uint32_t numResumeOffsets,
                                             uint32_t numScopeNotes,
                                             uint32_t numTryNotes) {
  Offset cursor = (*pcursor);

  // The byte arrays must have already been padded.
  MOZ_ASSERT(isAlignedOffset<CodeNoteAlign>(cursor),
             "Bytecode and source notes should be padded to keep alignment");

  // Each non-empty optional array needs will need an offset to its end.
  unsigned numOptionalArrays = unsigned(numResumeOffsets > 0) +
                               unsigned(numScopeNotes > 0) +
                               unsigned(numTryNotes > 0);

  // Default-initialize the optional-offsets.
  initElements<Offset>(cursor, numOptionalArrays);
  cursor += numOptionalArrays * sizeof(Offset);

  // Offset between optional-offsets table and the optional arrays. This is
  // later used to access the optional-offsets table as well as first optional
  // array.
  optArrayOffset_ = cursor;

  // Each optional array that follows must store an end-offset in the offset
  // table. Assign table entries by using this 'offsetIndex'. The index 0 is
  // reserved for implicit value 'optArrayOffset'.
  int offsetIndex = 0;

  // Default-initialize optional 'resumeOffsets'.
  MOZ_ASSERT(resumeOffsetsOffset() == cursor);
  if (numResumeOffsets > 0) {
    initElements<uint32_t>(cursor, numResumeOffsets);
    cursor += numResumeOffsets * sizeof(uint32_t);
    setOptionalOffset(++offsetIndex, cursor);
  }
  flagsRef().resumeOffsetsEndIndex = offsetIndex;

  // Default-initialize optional 'scopeNotes'.
  MOZ_ASSERT(scopeNotesOffset() == cursor);
  if (numScopeNotes > 0) {
    initElements<ScopeNote>(cursor, numScopeNotes);
    cursor += numScopeNotes * sizeof(ScopeNote);
    setOptionalOffset(++offsetIndex, cursor);
  }
  flagsRef().scopeNotesEndIndex = offsetIndex;

  // Default-initialize optional 'tryNotes'
  MOZ_ASSERT(tryNotesOffset() == cursor);
  if (numTryNotes > 0) {
    initElements<TryNote>(cursor, numTryNotes);
    cursor += numTryNotes * sizeof(TryNote);
    setOptionalOffset(++offsetIndex, cursor);
  }
  flagsRef().tryNotesEndIndex = offsetIndex;

  MOZ_ASSERT(endOffset() == cursor);
  (*pcursor) = cursor;
}

ImmutableScriptData::ImmutableScriptData(uint32_t codeLength,
                                         uint32_t noteLength,
                                         uint32_t numResumeOffsets,
                                         uint32_t numScopeNotes,
                                         uint32_t numTryNotes)
    : codeLength_(codeLength) {
  // Variable-length data begins immediately after ImmutableScriptData itself.
  Offset cursor = sizeof(ImmutableScriptData);

  // The following arrays are byte-aligned with additional padding to ensure
  // that together they maintain uint32_t-alignment.
  {
    MOZ_ASSERT(isAlignedOffset<CodeNoteAlign>(cursor));

    // Zero-initialize 'flags'
    MOZ_ASSERT(isAlignedOffset<Flags>(cursor));
    new (offsetToPointer<void>(cursor)) Flags{};
    cursor += sizeof(Flags);

    initElements<jsbytecode>(cursor, codeLength);
    cursor += codeLength * sizeof(jsbytecode);

    initElements<SrcNote>(cursor, noteLength);
    cursor += noteLength * sizeof(SrcNote);

    MOZ_ASSERT(isAlignedOffset<CodeNoteAlign>(cursor));
  }

  // Initialization for remaining arrays.
  initOptionalArrays(&cursor, numResumeOffsets, numScopeNotes, numTryNotes);

  // Check that we correctly recompute the expected values.
  MOZ_ASSERT(this->codeLength() == codeLength);
  MOZ_ASSERT(this->noteLength() == noteLength);

  // Sanity check
  MOZ_ASSERT(endOffset() == cursor);
}

template <XDRMode mode>
XDRResult js::XDRImmutableScriptData(XDRState<mode>* xdr,
                                     SharedImmutableScriptData& sisd) {
  static_assert(frontend::CanCopyDataToDisk<ImmutableScriptData>::value,
                "ImmutableScriptData cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<jsbytecode>::value,
                "jsbytecode cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<SrcNote>::value,
                "SrcNote cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<ScopeNote>::value,
                "ScopeNote cannot be bulk-copied to disk");
  static_assert(frontend::CanCopyDataToDisk<TryNote>::value,
                "TryNote cannot be bulk-copied to disk");

  uint32_t size;
  if (mode == XDR_ENCODE) {
    size = sisd.immutableDataLength();
  }
  MOZ_TRY(xdr->codeUint32(&size));

  MOZ_TRY(xdr->align32());
  static_assert(alignof(ImmutableScriptData) <= alignof(uint32_t));

  if (mode == XDR_ENCODE) {
    uint8_t* data = const_cast<uint8_t*>(sisd.get()->immutableData().data());
    MOZ_ASSERT(data == reinterpret_cast<const uint8_t*>(sisd.get()),
               "Decode below relies on the data placement");
    MOZ_TRY(xdr->codeBytes(data, size));
  } else {
    MOZ_ASSERT(!sisd.get());

    if (xdr->hasOptions() && xdr->options().usePinnedBytecode) {
      ImmutableScriptData* isd;
      MOZ_TRY(xdr->borrowedData(&isd, size));
      sisd.setExternal(isd);
    } else {
      auto isd = ImmutableScriptData::new_(xdr->cx(), size);
      if (!isd) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
      uint8_t* data = reinterpret_cast<uint8_t*>(isd.get());
      MOZ_TRY(xdr->codeBytes(data, size));
      sisd.setOwn(std::move(isd));
    }

    if (size != sisd.get()->computedSize()) {
      MOZ_ASSERT(false, "Bad ImmutableScriptData");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
    }
  }

  return Ok();
}

template XDRResult js::XDRImmutableScriptData(XDRState<XDR_ENCODE>* xdr,
                                              SharedImmutableScriptData& sisd);
template XDRResult js::XDRImmutableScriptData(XDRState<XDR_DECODE>* xdr,
                                              SharedImmutableScriptData& sisd);

template <XDRMode mode>
XDRResult js::XDRSourceExtent(XDRState<mode>* xdr, SourceExtent* extent) {
  MOZ_TRY(xdr->codeUint32(&extent->sourceStart));
  MOZ_TRY(xdr->codeUint32(&extent->sourceEnd));
  MOZ_TRY(xdr->codeUint32(&extent->toStringStart));
  MOZ_TRY(xdr->codeUint32(&extent->toStringEnd));
  MOZ_TRY(xdr->codeUint32(&extent->lineno));
  MOZ_TRY(xdr->codeUint32(&extent->column));

  return Ok();
}

template /* static */
    XDRResult
    js::XDRSourceExtent(XDRState<XDR_ENCODE>* xdr, SourceExtent* extent);

template /* static */
    XDRResult
    js::XDRSourceExtent(XDRState<XDR_DECODE>* xdr, SourceExtent* extent);

void js::FillImmutableFlagsFromCompileOptionsForTopLevel(
    const ReadOnlyCompileOptions& options, ImmutableScriptFlags& flags) {
  using ImmutableFlags = ImmutableScriptFlagsEnum;

  js::FillImmutableFlagsFromCompileOptionsForFunction(options, flags);

  flags.setFlag(ImmutableFlags::TreatAsRunOnce, options.isRunOnce);
  flags.setFlag(ImmutableFlags::NoScriptRval, options.noScriptRval);
}

void js::FillImmutableFlagsFromCompileOptionsForFunction(
    const ReadOnlyCompileOptions& options, ImmutableScriptFlags& flags) {
  using ImmutableFlags = ImmutableScriptFlagsEnum;

  flags.setFlag(ImmutableFlags::SelfHosted, options.selfHostingMode);
  flags.setFlag(ImmutableFlags::ForceStrict, options.forceStrictMode());
  flags.setFlag(ImmutableFlags::HasNonSyntacticScope,
                options.nonSyntacticScope);
}

// Check if flags matches to compile options for flags set by
// FillImmutableFlagsFromCompileOptionsForTopLevel above.
//
// If isMultiDecode is true, this check minimal set of CompileOptions that is
// shared across multiple scripts in JS::DecodeMultiOffThreadScripts.
// Other options should be checked when getting the decoded script from the
// cache.
bool js::CheckCompileOptionsMatch(const ReadOnlyCompileOptions& options,
                                  ImmutableScriptFlags flags,
                                  bool isMultiDecode) {
  using ImmutableFlags = ImmutableScriptFlagsEnum;

  bool selfHosted = !!(flags & uint32_t(ImmutableFlags::SelfHosted));
  bool forceStrict = !!(flags & uint32_t(ImmutableFlags::ForceStrict));
  bool hasNonSyntacticScope =
      !!(flags & uint32_t(ImmutableFlags::HasNonSyntacticScope));
  bool noScriptRval = !!(flags & uint32_t(ImmutableFlags::NoScriptRval));
  bool treatAsRunOnce = !!(flags & uint32_t(ImmutableFlags::TreatAsRunOnce));

  return options.selfHostingMode == selfHosted &&
         options.noScriptRval == noScriptRval &&
         options.isRunOnce == treatAsRunOnce &&
         (isMultiDecode || (options.forceStrictMode() == forceStrict &&
                            options.nonSyntacticScope == hasNonSyntacticScope));
}

JS_PUBLIC_API bool JS::CheckCompileOptionsMatch(
    const ReadOnlyCompileOptions& options, JSScript* script) {
  return js::CheckCompileOptionsMatch(options, script->immutableFlags(), false);
}

template <XDRMode mode>
XDRResult js::XDRScript(XDRState<mode>* xdr, HandleScope scriptEnclosingScope,
                        HandleScriptSourceObject sourceObjectArg,
                        HandleObject funOrMod, MutableHandleScript scriptp) {
  /* NB: Keep this in sync with CopyScriptImpl. */

  enum XDRScriptFlags {
    OwnSource = 1 << 0,
    HasLazyScript = 1 << 1,
  };

  uint8_t xdrFlags = 0;

  SourceExtent extent;
  uint32_t immutableFlags = 0;

  // NOTE: |mutableFlags| are not preserved by XDR.

  JSContext* cx = xdr->cx();
  RootedScript script(cx);

  bool isFunctionScript = funOrMod && funOrMod->is<JSFunction>();

  if (mode == XDR_ENCODE) {
    script = scriptp.get();

    MOZ_ASSERT_IF(isFunctionScript, script->function() == funOrMod);

    if (!sourceObjectArg) {
      xdrFlags |= OwnSource;
    }
    // Preserve the MutableFlags::AllowRelazify flag.
    if (script->allowRelazify()) {
      xdrFlags |= HasLazyScript;
    }
  }

  MOZ_TRY(xdr->codeUint8(&xdrFlags));

  if (mode == XDR_ENCODE) {
    extent = script->extent();
    immutableFlags = script->immutableFlags();
  }

  MOZ_TRY(XDRSourceExtent(xdr, &extent));
  MOZ_TRY(xdr->codeUint32(&immutableFlags));

  RootedScriptSourceObject sourceObject(cx, sourceObjectArg);
  Maybe<CompileOptions> options;

  if (mode == XDR_DECODE) {
    MOZ_ASSERT(xdr->hasOptions());

    // When loading from the bytecode cache, and if we get the CompileOptions
    // from the document, if the ImmutableFlags and options don't agree, we
    // should fail. This only applies to the top-level and not its inner
    // functions.
    //
    // Also, JS::DecodeMultiOffThreadScripts uses single CompileOptions for
    // multiple scripts with different CompileOptions.
    // We should check minimal set of common flags here, and let the consumer
    // check the full flags when getting from the cache.
    if (xdrFlags & OwnSource) {
      options.emplace(xdr->cx(), xdr->options());
      if (!js::CheckCompileOptionsMatch(*options,
                                        ImmutableScriptFlags(immutableFlags),
                                        xdr->isMultiDecode())) {
        return xdr->fail(JS::TranscodeResult::Failure_WrongCompileOption);
      }
    }
  }

  if (xdrFlags & OwnSource) {
    RefPtr<ScriptSource> source;

    // We are relying on the script's ScriptSource so the caller should not
    // have passed in an explicit one.
    MOZ_ASSERT(sourceObjectArg == nullptr);

    if (mode == XDR_ENCODE) {
      sourceObject = script->sourceObject();
      source = do_AddRef(sourceObject->source());
    }

    MOZ_TRY(ScriptSource::XDR(xdr, options.ptrOr(nullptr), source));

    if (mode == XDR_DECODE) {
      sourceObject = ScriptSourceObject::create(cx, source);
      if (!sourceObject) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      if (xdr->hasScriptSourceObjectOut()) {
        // When the ScriptSourceObjectOut is provided by ParseTask, it
        // is stored in a location which is traced by the GC.
        *xdr->scriptSourceObjectOut() = sourceObject;
      } else if (!ScriptSourceObject::initFromOptions(cx, sourceObject,
                                                      *options)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  } else {
    // While encoding, the ScriptSource passed in must match the ScriptSource
    // of the script.
    MOZ_ASSERT_IF(mode == XDR_ENCODE,
                  sourceObjectArg->source() == script->scriptSource());
  }

  if (mode == XDR_DECODE) {
    RootedObject functionOrGlobal(
        cx, isFunctionScript ? static_cast<JSObject*>(funOrMod)
                             : static_cast<JSObject*>(cx->global()));

    script = JSScript::Create(cx, functionOrGlobal, sourceObject, extent,
                              ImmutableScriptFlags(immutableFlags));
    if (!script) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    scriptp.set(script);

    // Set the script in its function now so that inner scripts to be
    // decoded may iterate the static scope chain.
    if (isFunctionScript) {
      funOrMod->as<JSFunction>().initScript(script);
    }
  }

  // If XDR operation fails, we must call BaseScript::freeSharedData in order to
  // neuter the script. Various things that iterate raw scripts in a GC arena
  // use the presense of this data to detect if initialization is complete.
  auto scriptDataGuard = mozilla::MakeScopeExit([&] {
    if (mode == XDR_DECODE) {
      script->freeSharedData();
    }
  });

  // NOTE: The script data is rooted by the script.
  MOZ_TRY(PrivateScriptData::XDR<mode>(xdr, script, sourceObject,
                                       scriptEnclosingScope, funOrMod));
  MOZ_TRY(frontend::StencilXDR::codeSharedData<mode>(xdr, script->sharedData_));

  if (xdrFlags & HasLazyScript) {
    if (mode == XDR_DECODE) {
      script->setAllowRelazify();
    }
  }

  if (mode == XDR_DECODE) {
    if (coverage::IsLCovEnabled()) {
      if (!coverage::InitScriptCoverage(cx, script)) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }

    /* see BytecodeEmitter::tellDebuggerAboutCompiledScript */
    if (!isFunctionScript && !cx->isHelperThreadContext() &&
        !xdr->options().hideFromNewScriptInitial()) {
      DebugAPI::onNewScript(cx, script);
    }
  }

  MOZ_ASSERT(script->code(), "Where's our bytecode?");
  scriptDataGuard.release();
  return Ok();
}

template XDRResult js::XDRScript(XDRState<XDR_ENCODE>*, HandleScope,
                                 HandleScriptSourceObject, HandleObject,
                                 MutableHandleScript);

template XDRResult js::XDRScript(XDRState<XDR_DECODE>*, HandleScope,
                                 HandleScriptSourceObject, HandleObject,
                                 MutableHandleScript);

template <XDRMode mode>
XDRResult js::XDRLazyScript(XDRState<mode>* xdr, HandleScope enclosingScope,
                            HandleScriptSourceObject sourceObject,
                            HandleFunction fun,
                            MutableHandle<BaseScript*> lazy) {
  MOZ_ASSERT_IF(mode == XDR_DECODE, sourceObject);

  JSContext* cx = xdr->cx();

  {
    SourceExtent extent;
    uint32_t immutableFlags;
    uint32_t ngcthings;

    if (mode == XDR_ENCODE) {
      MOZ_ASSERT(fun == lazy->function());

      extent = lazy->extent();
      immutableFlags = lazy->immutableFlags();
      ngcthings = lazy->gcthings().size();
    }

    MOZ_TRY(XDRSourceExtent(xdr, &extent));
    MOZ_TRY(xdr->codeUint32(&immutableFlags));
    MOZ_TRY(xdr->codeUint32(&ngcthings));

    if (mode == XDR_DECODE) {
      lazy.set(BaseScript::CreateRawLazy(cx, ngcthings, fun, sourceObject,
                                         extent, immutableFlags));
      if (!lazy) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }

      // Set the enclosing scope of the lazy function. This value should only be
      // set if we have a non-lazy enclosing script at this point.
      // BaseScript::enclosingScriptHasEverBeenCompiled relies on the enclosing
      // scope being non-null if we have ever been nested inside non-lazy
      // function.
      if (enclosingScope) {
        lazy->setEnclosingScope(enclosingScope);
      }

      fun->initScript(lazy);
    }
  }

  MOZ_TRY(BaseScript::XDRLazyScriptData(xdr, sourceObject, lazy));

  return Ok();
}

template XDRResult js::XDRLazyScript(XDRState<XDR_ENCODE>*, HandleScope,
                                     HandleScriptSourceObject, HandleFunction,
                                     MutableHandle<BaseScript*>);

template XDRResult js::XDRLazyScript(XDRState<XDR_DECODE>*, HandleScope,
                                     HandleScriptSourceObject, HandleFunction,
                                     MutableHandle<BaseScript*>);

bool JSScript::initScriptCounts(JSContext* cx) {
  MOZ_ASSERT(!hasScriptCounts());

  // Record all pc which are the first instruction of a basic block.
  mozilla::Vector<jsbytecode*, 16, SystemAllocPolicy> jumpTargets;

  js::BytecodeLocation main = mainLocation();
  AllBytecodesIterable iterable(this);
  for (auto& loc : iterable) {
    if (loc.isJumpTarget() || loc == main) {
      if (!jumpTargets.append(loc.toRawBytecode())) {
        ReportOutOfMemory(cx);
        return false;
      }
    }
  }

  // Initialize all PCCounts counters to 0.
  ScriptCounts::PCCountsVector base;
  if (!base.reserve(jumpTargets.length())) {
    ReportOutOfMemory(cx);
    return false;
  }

  for (size_t i = 0; i < jumpTargets.length(); i++) {
    base.infallibleEmplaceBack(pcToOffset(jumpTargets[i]));
  }

  // Create zone's scriptCountsMap if necessary.
  if (!zone()->scriptCountsMap) {
    auto map = cx->make_unique<ScriptCountsMap>();
    if (!map) {
      return false;
    }

    zone()->scriptCountsMap = std::move(map);
  }

  // Allocate the ScriptCounts.
  UniqueScriptCounts sc = cx->make_unique<ScriptCounts>(std::move(base));
  if (!sc) {
    ReportOutOfMemory(cx);
    return false;
  }

  MOZ_ASSERT(this->hasBytecode());

  // Register the current ScriptCounts in the zone's map.
  if (!zone()->scriptCountsMap->putNew(this, std::move(sc))) {
    ReportOutOfMemory(cx);
    return false;
  }

  // safe to set this;  we can't fail after this point.
  setHasScriptCounts();

  // Enable interrupts in any interpreter frames running on this script. This
  // is used to let the interpreter increment the PCCounts, if present.
  for (ActivationIterator iter(cx); !iter.done(); ++iter) {
    if (iter->isInterpreter()) {
      iter->asInterpreter()->enableInterruptsIfRunning(this);
    }
  }

  return true;
}

static inline ScriptCountsMap::Ptr GetScriptCountsMapEntry(JSScript* script) {
  MOZ_ASSERT(script->hasScriptCounts());
  ScriptCountsMap::Ptr p = script->zone()->scriptCountsMap->lookup(script);
  MOZ_ASSERT(p);
  return p;
}

ScriptCounts& JSScript::getScriptCounts() {
  ScriptCountsMap::Ptr p = GetScriptCountsMapEntry(this);
  return *p->value();
}

js::PCCounts* ScriptCounts::maybeGetPCCounts(size_t offset) {
  PCCounts searched = PCCounts(offset);
  PCCounts* elem =
      std::lower_bound(pcCounts_.begin(), pcCounts_.end(), searched);
  if (elem == pcCounts_.end() || elem->pcOffset() != offset) {
    return nullptr;
  }
  return elem;
}

const js::PCCounts* ScriptCounts::maybeGetPCCounts(size_t offset) const {
  PCCounts searched = PCCounts(offset);
  const PCCounts* elem =
      std::lower_bound(pcCounts_.begin(), pcCounts_.end(), searched);
  if (elem == pcCounts_.end() || elem->pcOffset() != offset) {
    return nullptr;
  }
  return elem;
}

js::PCCounts* ScriptCounts::getImmediatePrecedingPCCounts(size_t offset) {
  PCCounts searched = PCCounts(offset);
  PCCounts* elem =
      std::lower_bound(pcCounts_.begin(), pcCounts_.end(), searched);
  if (elem == pcCounts_.end()) {
    return &pcCounts_.back();
  }
  if (elem->pcOffset() == offset) {
    return elem;
  }
  if (elem != pcCounts_.begin()) {
    return elem - 1;
  }
  return nullptr;
}

const js::PCCounts* ScriptCounts::maybeGetThrowCounts(size_t offset) const {
  PCCounts searched = PCCounts(offset);
  const PCCounts* elem =
      std::lower_bound(throwCounts_.begin(), throwCounts_.end(), searched);
  if (elem == throwCounts_.end() || elem->pcOffset() != offset) {
    return nullptr;
  }
  return elem;
}

const js::PCCounts* ScriptCounts::getImmediatePrecedingThrowCounts(
    size_t offset) const {
  PCCounts searched = PCCounts(offset);
  const PCCounts* elem =
      std::lower_bound(throwCounts_.begin(), throwCounts_.end(), searched);
  if (elem == throwCounts_.end()) {
    if (throwCounts_.begin() == throwCounts_.end()) {
      return nullptr;
    }
    return &throwCounts_.back();
  }
  if (elem->pcOffset() == offset) {
    return elem;
  }
  if (elem != throwCounts_.begin()) {
    return elem - 1;
  }
  return nullptr;
}

js::PCCounts* ScriptCounts::getThrowCounts(size_t offset) {
  PCCounts searched = PCCounts(offset);
  PCCounts* elem =
      std::lower_bound(throwCounts_.begin(), throwCounts_.end(), searched);
  if (elem == throwCounts_.end() || elem->pcOffset() != offset) {
    elem = throwCounts_.insert(elem, searched);
  }
  return elem;
}

size_t ScriptCounts::sizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf) {
  size_t size = mallocSizeOf(this);
  size += pcCounts_.sizeOfExcludingThis(mallocSizeOf);
  size += throwCounts_.sizeOfExcludingThis(mallocSizeOf);
  if (ionCounts_) {
    size += ionCounts_->sizeOfIncludingThis(mallocSizeOf);
  }
  return size;
}

js::PCCounts* JSScript::maybeGetPCCounts(jsbytecode* pc) {
  MOZ_ASSERT(containsPC(pc));
  return getScriptCounts().maybeGetPCCounts(pcToOffset(pc));
}

const js::PCCounts* JSScript::maybeGetThrowCounts(jsbytecode* pc) {
  MOZ_ASSERT(containsPC(pc));
  return getScriptCounts().maybeGetThrowCounts(pcToOffset(pc));
}

js::PCCounts* JSScript::getThrowCounts(jsbytecode* pc) {
  MOZ_ASSERT(containsPC(pc));
  return getScriptCounts().getThrowCounts(pcToOffset(pc));
}

uint64_t JSScript::getHitCount(jsbytecode* pc) {
  MOZ_ASSERT(containsPC(pc));
  if (pc < main()) {
    pc = main();
  }

  ScriptCounts& sc = getScriptCounts();
  size_t targetOffset = pcToOffset(pc);
  const js::PCCounts* baseCount =
      sc.getImmediatePrecedingPCCounts(targetOffset);
  if (!baseCount) {
    return 0;
  }
  if (baseCount->pcOffset() == targetOffset) {
    return baseCount->numExec();
  }
  MOZ_ASSERT(baseCount->pcOffset() < targetOffset);
  uint64_t count = baseCount->numExec();
  do {
    const js::PCCounts* throwCount =
        sc.getImmediatePrecedingThrowCounts(targetOffset);
    if (!throwCount) {
      return count;
    }
    if (throwCount->pcOffset() <= baseCount->pcOffset()) {
      return count;
    }
    count -= throwCount->numExec();
    targetOffset = throwCount->pcOffset() - 1;
  } while (true);
}

void JSScript::addIonCounts(jit::IonScriptCounts* ionCounts) {
  ScriptCounts& sc = getScriptCounts();
  if (sc.ionCounts_) {
    ionCounts->setPrevious(sc.ionCounts_);
  }
  sc.ionCounts_ = ionCounts;
}

jit::IonScriptCounts* JSScript::getIonCounts() {
  return getScriptCounts().ionCounts_;
}

void JSScript::releaseScriptCounts(ScriptCounts* counts) {
  ScriptCountsMap::Ptr p = GetScriptCountsMapEntry(this);
  *counts = std::move(*p->value().get());
  zone()->scriptCountsMap->remove(p);
  clearHasScriptCounts();
}

void JSScript::destroyScriptCounts() {
  if (hasScriptCounts()) {
    ScriptCounts scriptCounts;
    releaseScriptCounts(&scriptCounts);
  }
}

void JSScript::resetScriptCounts() {
  if (!hasScriptCounts()) {
    return;
  }

  ScriptCounts& sc = getScriptCounts();

  for (PCCounts& elem : sc.pcCounts_) {
    elem.numExec() = 0;
  }

  for (PCCounts& elem : sc.throwCounts_) {
    elem.numExec() = 0;
  }
}

void ScriptSourceObject::finalize(JSFreeOp* fop, JSObject* obj) {
  MOZ_ASSERT(fop->onMainThread());
  ScriptSourceObject* sso = &obj->as<ScriptSourceObject>();
  if (sso->isCanonical()) {
    sso->source()->finalizeGCData();
  }
  sso->source()->Release();

  // Clear the private value, calling the release hook if necessary.
  sso->setPrivate(fop->runtime(), UndefinedValue());
}

static const JSClassOps ScriptSourceObjectClassOps = {
    nullptr,                       // addProperty
    nullptr,                       // delProperty
    nullptr,                       // enumerate
    nullptr,                       // newEnumerate
    nullptr,                       // resolve
    nullptr,                       // mayResolve
    ScriptSourceObject::finalize,  // finalize
    nullptr,                       // call
    nullptr,                       // hasInstance
    nullptr,                       // construct
    nullptr,                       // trace
};

const JSClass ScriptSourceObject::class_ = {
    "ScriptSource",
    JSCLASS_HAS_RESERVED_SLOTS(RESERVED_SLOTS) | JSCLASS_FOREGROUND_FINALIZE,
    &ScriptSourceObjectClassOps};

ScriptSourceObject* ScriptSourceObject::createInternal(JSContext* cx,
                                                       ScriptSource* source,
                                                       HandleObject canonical) {
  ScriptSourceObject* obj =
      NewObjectWithGivenProto<ScriptSourceObject>(cx, nullptr);
  if (!obj) {
    return nullptr;
  }

  // The matching decref is in ScriptSourceObject::finalize.
  obj->initReservedSlot(SOURCE_SLOT, PrivateValue(do_AddRef(source).take()));

  if (canonical) {
    obj->initReservedSlot(CANONICAL_SLOT, ObjectValue(*canonical));
  } else {
    obj->initReservedSlot(CANONICAL_SLOT, ObjectValue(*obj));
  }

  // The slots below should either be populated by a call to initFromOptions or,
  // if this is a non-canonical ScriptSourceObject, they are unused. Poison
  // them.
  obj->initReservedSlot(ELEMENT_PROPERTY_SLOT, MagicValue(JS_GENERIC_MAGIC));
  obj->initReservedSlot(INTRODUCTION_SCRIPT_SLOT, MagicValue(JS_GENERIC_MAGIC));

  return obj;
}

ScriptSourceObject* ScriptSourceObject::create(JSContext* cx,
                                               ScriptSource* source) {
  return createInternal(cx, source, nullptr);
}

ScriptSourceObject* ScriptSourceObject::clone(JSContext* cx,
                                              HandleScriptSourceObject sso) {
  MOZ_ASSERT(cx->compartment() != sso->compartment());

  RootedObject wrapped(cx, sso);
  if (!cx->compartment()->wrap(cx, &wrapped)) {
    return nullptr;
  }

  return createInternal(cx, sso->source(), wrapped);
}

ScriptSourceObject* ScriptSourceObject::unwrappedCanonical() const {
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(runtimeFromAnyThread()));

  JSObject* obj = &getReservedSlot(CANONICAL_SLOT).toObject();
  return &UncheckedUnwrap(obj)->as<ScriptSourceObject>();
}

[[nodiscard]] static bool MaybeValidateFilename(
    JSContext* cx, HandleScriptSourceObject sso,
    const ReadOnlyCompileOptions& options) {
  // When parsing off-thread we want to do filename validation on the main
  // thread. This makes off-thread parsing more pure and is simpler because we
  // can't easily throw exceptions off-thread.
  MOZ_ASSERT(!cx->isHelperThreadContext());

  if (!gFilenameValidationCallback) {
    return true;
  }

  const char* filename = sso->source()->filename();
  if (!filename || options.skipFilenameValidation()) {
    return true;
  }

  if (gFilenameValidationCallback(filename, cx->realm()->isSystem())) {
    return true;
  }

  const char* utf8Filename;
  if (mozilla::IsUtf8(mozilla::MakeStringSpan(filename))) {
    utf8Filename = filename;
  } else {
    utf8Filename = "(invalid UTF-8 filename)";
  }
  JS_ReportErrorNumberUTF8(cx, GetErrorMessage, nullptr, JSMSG_UNSAFE_FILENAME,
                           utf8Filename);
  return false;
}

/* static */
bool ScriptSourceObject::initFromOptions(
    JSContext* cx, HandleScriptSourceObject source,
    const ReadOnlyCompileOptions& options) {
  cx->releaseCheck(source);
  MOZ_ASSERT(source->isCanonical());
  MOZ_ASSERT(
      source->getReservedSlot(ELEMENT_PROPERTY_SLOT).isMagic(JS_GENERIC_MAGIC));
  MOZ_ASSERT(source->getReservedSlot(INTRODUCTION_SCRIPT_SLOT)
                 .isMagic(JS_GENERIC_MAGIC));

  if (!MaybeValidateFilename(cx, source, options)) {
    return false;
  }

  if (options.deferDebugMetadata) {
    return true;
  }

  // Initialize the element attribute slot and introduction script slot
  // this marks the SSO as initialized for asserts.

  RootedString elementAttributeName(cx);
  if (!initElementProperties(cx, source, elementAttributeName)) {
    return false;
  }

  RootedValue introductionScript(cx);
  source->setReservedSlot(INTRODUCTION_SCRIPT_SLOT, introductionScript);

  return true;
}

/* static */
bool ScriptSourceObject::initElementProperties(JSContext* cx,
                                               HandleScriptSourceObject source,
                                               HandleString elementAttrName) {
  MOZ_ASSERT(source->isCanonical());

  RootedValue nameValue(cx);
  if (elementAttrName) {
    nameValue = StringValue(elementAttrName);
  }
  if (!cx->compartment()->wrap(cx, &nameValue)) {
    return false;
  }

  source->setReservedSlot(ELEMENT_PROPERTY_SLOT, nameValue);

  return true;
}

void ScriptSourceObject::setPrivate(JSRuntime* rt, const Value& value) {
  // Update the private value, calling addRef/release hooks if necessary
  // to allow the embedding to maintain a reference count for the
  // private data.
  JS::AutoSuppressGCAnalysis nogc;
  Value prevValue = getReservedSlot(PRIVATE_SLOT);
  rt->releaseScriptPrivate(prevValue);
  setReservedSlot(PRIVATE_SLOT, value);
  rt->addRefScriptPrivate(value);
}

JSObject* ScriptSourceObject::unwrappedElement(JSContext* cx) const {
  JS::RootedValue privateValue(cx, unwrappedCanonical()->canonicalPrivate());
  if (privateValue.isUndefined()) {
    return nullptr;
  }

  if (cx->runtime()->sourceElementCallback) {
    return (*cx->runtime()->sourceElementCallback)(cx, privateValue);
  }

  return nullptr;
}

class ScriptSource::LoadSourceMatcher {
  JSContext* const cx_;
  ScriptSource* const ss_;
  bool* const loaded_;

 public:
  explicit LoadSourceMatcher(JSContext* cx, ScriptSource* ss, bool* loaded)
      : cx_(cx), ss_(ss), loaded_(loaded) {}

  template <typename Unit, SourceRetrievable CanRetrieve>
  bool operator()(const Compressed<Unit, CanRetrieve>&) const {
    *loaded_ = true;
    return true;
  }

  template <typename Unit, SourceRetrievable CanRetrieve>
  bool operator()(const Uncompressed<Unit, CanRetrieve>&) const {
    *loaded_ = true;
    return true;
  }

  template <typename Unit>
  bool operator()(const Retrievable<Unit>&) {
    if (!cx_->runtime()->sourceHook.ref()) {
      *loaded_ = false;
      return true;
    }

    size_t length;

    // The first argument is just for overloading -- its value doesn't matter.
    if (!tryLoadAndSetSource(Unit('0'), &length)) {
      return false;
    }

    return true;
  }

  bool operator()(const Missing&) const {
    *loaded_ = false;
    return true;
  }

 private:
  bool tryLoadAndSetSource(const Utf8Unit&, size_t* length) const {
    char* utf8Source;
    if (!cx_->runtime()->sourceHook->load(cx_, ss_->filename(), nullptr,
                                          &utf8Source, length)) {
      return false;
    }

    if (!utf8Source) {
      *loaded_ = false;
      return true;
    }

    if (!ss_->setRetrievedSource(
            cx_, EntryUnits<Utf8Unit>(reinterpret_cast<Utf8Unit*>(utf8Source)),
            *length)) {
      return false;
    }

    *loaded_ = true;
    return true;
  }

  bool tryLoadAndSetSource(const char16_t&, size_t* length) const {
    char16_t* utf16Source;
    if (!cx_->runtime()->sourceHook->load(cx_, ss_->filename(), &utf16Source,
                                          nullptr, length)) {
      return false;
    }

    if (!utf16Source) {
      *loaded_ = false;
      return true;
    }

    if (!ss_->setRetrievedSource(cx_, EntryUnits<char16_t>(utf16Source),
                                 *length)) {
      return false;
    }

    *loaded_ = true;
    return true;
  }
};

/* static */
bool ScriptSource::loadSource(JSContext* cx, ScriptSource* ss, bool* loaded) {
  return ss->data.match(LoadSourceMatcher(cx, ss, loaded));
}

/* static */
JSLinearString* JSScript::sourceData(JSContext* cx, HandleScript script) {
  MOZ_ASSERT(script->scriptSource()->hasSourceText());
  return script->scriptSource()->substring(cx, script->sourceStart(),
                                           script->sourceEnd());
}

bool BaseScript::appendSourceDataForToString(JSContext* cx, StringBuffer& buf) {
  MOZ_ASSERT(scriptSource()->hasSourceText());
  return scriptSource()->appendSubstring(cx, buf, toStringStart(),
                                         toStringEnd());
}

void UncompressedSourceCache::holdEntry(AutoHoldEntry& holder,
                                        const ScriptSourceChunk& ssc) {
  MOZ_ASSERT(!holder_);
  holder.holdEntry(this, ssc);
  holder_ = &holder;
}

void UncompressedSourceCache::releaseEntry(AutoHoldEntry& holder) {
  MOZ_ASSERT(holder_ == &holder);
  holder_ = nullptr;
}

template <typename Unit>
const Unit* UncompressedSourceCache::lookup(const ScriptSourceChunk& ssc,
                                            AutoHoldEntry& holder) {
  MOZ_ASSERT(!holder_);
  MOZ_ASSERT(ssc.ss->isCompressed<Unit>());

  if (!map_) {
    return nullptr;
  }

  if (Map::Ptr p = map_->lookup(ssc)) {
    holdEntry(holder, ssc);
    return static_cast<const Unit*>(p->value().get());
  }

  return nullptr;
}

bool UncompressedSourceCache::put(const ScriptSourceChunk& ssc, SourceData data,
                                  AutoHoldEntry& holder) {
  MOZ_ASSERT(!holder_);

  if (!map_) {
    map_ = MakeUnique<Map>();
    if (!map_) {
      return false;
    }
  }

  if (!map_->put(ssc, std::move(data))) {
    return false;
  }

  holdEntry(holder, ssc);
  return true;
}

void UncompressedSourceCache::purge() {
  if (!map_) {
    return;
  }

  for (Map::Range r = map_->all(); !r.empty(); r.popFront()) {
    if (holder_ && r.front().key() == holder_->sourceChunk()) {
      holder_->deferDelete(std::move(r.front().value()));
      holder_ = nullptr;
    }
  }

  map_ = nullptr;
}

size_t UncompressedSourceCache::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) {
  size_t n = 0;
  if (map_ && !map_->empty()) {
    n += map_->shallowSizeOfIncludingThis(mallocSizeOf);
    for (Map::Range r = map_->all(); !r.empty(); r.popFront()) {
      n += mallocSizeOf(r.front().value().get());
    }
  }
  return n;
}

template <typename Unit>
const Unit* ScriptSource::chunkUnits(
    JSContext* cx, UncompressedSourceCache::AutoHoldEntry& holder,
    size_t chunk) {
  const CompressedData<Unit>& c = *compressedData<Unit>();

  ScriptSourceChunk ssc(this, chunk);
  if (const Unit* decompressed =
          cx->caches().uncompressedSourceCache.lookup<Unit>(ssc, holder)) {
    return decompressed;
  }

  size_t totalLengthInBytes = length() * sizeof(Unit);
  size_t chunkBytes = Compressor::chunkSize(totalLengthInBytes, chunk);

  MOZ_ASSERT((chunkBytes % sizeof(Unit)) == 0);
  const size_t chunkLength = chunkBytes / sizeof(Unit);
  EntryUnits<Unit> decompressed(js_pod_malloc<Unit>(chunkLength));
  if (!decompressed) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  // Compression treats input and output memory as plain ol' bytes. These
  // reinterpret_cast<>s accord exactly with that.
  if (!DecompressStringChunk(
          reinterpret_cast<const unsigned char*>(c.raw.chars()), chunk,
          reinterpret_cast<unsigned char*>(decompressed.get()), chunkBytes)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  const Unit* ret = decompressed.get();
  if (!cx->caches().uncompressedSourceCache.put(
          ssc, ToSourceData(std::move(decompressed)), holder)) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }
  return ret;
}

template <typename Unit>
void ScriptSource::convertToCompressedSource(SharedImmutableString compressed,
                                             size_t uncompressedLength) {
  MOZ_ASSERT(isUncompressed<Unit>());
  MOZ_ASSERT(uncompressedData<Unit>()->length() == uncompressedLength);

  if (data.is<Uncompressed<Unit, SourceRetrievable::Yes>>()) {
    data = SourceType(Compressed<Unit, SourceRetrievable::Yes>(
        std::move(compressed), uncompressedLength));
  } else {
    data = SourceType(Compressed<Unit, SourceRetrievable::No>(
        std::move(compressed), uncompressedLength));
  }
}

template <typename Unit>
void ScriptSource::performDelayedConvertToCompressedSource() {
  // There might not be a conversion to compressed source happening at all.
  if (pendingCompressed_.empty()) {
    return;
  }

  CompressedData<Unit>& pending =
      pendingCompressed_.ref<CompressedData<Unit>>();

  convertToCompressedSource<Unit>(std::move(pending.raw),
                                  pending.uncompressedLength);

  pendingCompressed_.destroy();
}

template <typename Unit>
ScriptSource::PinnedUnits<Unit>::~PinnedUnits() {
  if (units_) {
    MOZ_ASSERT(*stack_ == this);
    *stack_ = prev_;
    if (!prev_) {
      source_->performDelayedConvertToCompressedSource<Unit>();
    }
  }
}

template <typename Unit>
const Unit* ScriptSource::units(JSContext* cx,
                                UncompressedSourceCache::AutoHoldEntry& holder,
                                size_t begin, size_t len) {
  MOZ_ASSERT(begin <= length());
  MOZ_ASSERT(begin + len <= length());

  if (isUncompressed<Unit>()) {
    const Unit* units = uncompressedData<Unit>()->units();
    if (!units) {
      return nullptr;
    }
    return units + begin;
  }

  if (data.is<Missing>()) {
    MOZ_CRASH("ScriptSource::units() on ScriptSource with missing source");
  }

  if (data.is<Retrievable<Unit>>()) {
    MOZ_CRASH("ScriptSource::units() on ScriptSource with retrievable source");
  }

  MOZ_ASSERT(isCompressed<Unit>());

  // Determine first/last chunks, the offset (in bytes) into the first chunk
  // of the requested units, and the number of bytes in the last chunk.
  //
  // Note that first and last chunk sizes are miscomputed and *must not be
  // used* when the first chunk is the last chunk.
  size_t firstChunk, firstChunkOffset, firstChunkSize;
  size_t lastChunk, lastChunkSize;
  Compressor::rangeToChunkAndOffset(
      begin * sizeof(Unit), (begin + len) * sizeof(Unit), &firstChunk,
      &firstChunkOffset, &firstChunkSize, &lastChunk, &lastChunkSize);
  MOZ_ASSERT(firstChunk <= lastChunk);
  MOZ_ASSERT(firstChunkOffset % sizeof(Unit) == 0);
  MOZ_ASSERT(firstChunkSize % sizeof(Unit) == 0);

  size_t firstUnit = firstChunkOffset / sizeof(Unit);

  // Directly return units within a single chunk.  UncompressedSourceCache
  // and |holder| will hold the units alive past function return.
  if (firstChunk == lastChunk) {
    const Unit* units = chunkUnits<Unit>(cx, holder, firstChunk);
    if (!units) {
      return nullptr;
    }

    return units + firstUnit;
  }

  // Otherwise the units span multiple chunks.  Copy successive chunks'
  // decompressed units into freshly-allocated memory to return.
  EntryUnits<Unit> decompressed(js_pod_malloc<Unit>(len));
  if (!decompressed) {
    JS_ReportOutOfMemory(cx);
    return nullptr;
  }

  Unit* cursor;

  {
    // |AutoHoldEntry| is single-shot, and a holder successfully filled in
    // by |chunkUnits| must be destroyed before another can be used.  Thus
    // we can't use |holder| with |chunkUnits| when |chunkUnits| is used
    // with multiple chunks, and we must use and destroy distinct, fresh
    // holders for each chunk.
    UncompressedSourceCache::AutoHoldEntry firstHolder;
    const Unit* units = chunkUnits<Unit>(cx, firstHolder, firstChunk);
    if (!units) {
      return nullptr;
    }

    cursor = std::copy_n(units + firstUnit, firstChunkSize / sizeof(Unit),
                         decompressed.get());
  }

  for (size_t i = firstChunk + 1; i < lastChunk; i++) {
    UncompressedSourceCache::AutoHoldEntry chunkHolder;
    const Unit* units = chunkUnits<Unit>(cx, chunkHolder, i);
    if (!units) {
      return nullptr;
    }

    cursor = std::copy_n(units, Compressor::CHUNK_SIZE / sizeof(Unit), cursor);
  }

  {
    UncompressedSourceCache::AutoHoldEntry lastHolder;
    const Unit* units = chunkUnits<Unit>(cx, lastHolder, lastChunk);
    if (!units) {
      return nullptr;
    }

    cursor = std::copy_n(units, lastChunkSize / sizeof(Unit), cursor);
  }

  MOZ_ASSERT(PointerRangeSize(decompressed.get(), cursor) == len);

  // Transfer ownership to |holder|.
  const Unit* ret = decompressed.get();
  holder.holdUnits(std::move(decompressed));
  return ret;
}

template <typename Unit>
ScriptSource::PinnedUnits<Unit>::PinnedUnits(
    JSContext* cx, ScriptSource* source,
    UncompressedSourceCache::AutoHoldEntry& holder, size_t begin, size_t len)
    : PinnedUnitsBase(source) {
  MOZ_ASSERT(source->hasSourceType<Unit>(), "must pin units of source's type");

  units_ = source->units<Unit>(cx, holder, begin, len);
  if (units_) {
    stack_ = &source->pinnedUnitsStack_;
    prev_ = *stack_;
    *stack_ = this;
  }
}

template class ScriptSource::PinnedUnits<Utf8Unit>;
template class ScriptSource::PinnedUnits<char16_t>;

JSLinearString* ScriptSource::substring(JSContext* cx, size_t start,
                                        size_t stop) {
  MOZ_ASSERT(start <= stop);

  size_t len = stop - start;
  if (!len) {
    return cx->emptyString();
  }
  UncompressedSourceCache::AutoHoldEntry holder;

  // UTF-8 source text.
  if (hasSourceType<Utf8Unit>()) {
    PinnedUnits<Utf8Unit> units(cx, this, holder, start, len);
    if (!units.asChars()) {
      return nullptr;
    }

    const char* str = units.asChars();
    return NewStringCopyUTF8N<CanGC>(cx, JS::UTF8Chars(str, len));
  }

  // UTF-16 source text.
  PinnedUnits<char16_t> units(cx, this, holder, start, len);
  if (!units.asChars()) {
    return nullptr;
  }

  return NewStringCopyN<CanGC>(cx, units.asChars(), len);
}

JSLinearString* ScriptSource::substringDontDeflate(JSContext* cx, size_t start,
                                                   size_t stop) {
  MOZ_ASSERT(start <= stop);

  size_t len = stop - start;
  if (!len) {
    return cx->emptyString();
  }
  UncompressedSourceCache::AutoHoldEntry holder;

  // UTF-8 source text.
  if (hasSourceType<Utf8Unit>()) {
    PinnedUnits<Utf8Unit> units(cx, this, holder, start, len);
    if (!units.asChars()) {
      return nullptr;
    }

    const char* str = units.asChars();

    // There doesn't appear to be a non-deflating UTF-8 string creation
    // function -- but then again, it's not entirely clear how current
    // callers benefit from non-deflation.
    return NewStringCopyUTF8N<CanGC>(cx, JS::UTF8Chars(str, len));
  }

  // UTF-16 source text.
  PinnedUnits<char16_t> units(cx, this, holder, start, len);
  if (!units.asChars()) {
    return nullptr;
  }

  return NewStringCopyNDontDeflate<CanGC>(cx, units.asChars(), len);
}

bool ScriptSource::appendSubstring(JSContext* cx, StringBuffer& buf,
                                   size_t start, size_t stop) {
  MOZ_ASSERT(start <= stop);

  size_t len = stop - start;
  UncompressedSourceCache::AutoHoldEntry holder;

  if (hasSourceType<Utf8Unit>()) {
    PinnedUnits<Utf8Unit> pinned(cx, this, holder, start, len);
    if (!pinned.get()) {
      return false;
    }
    if (len > SourceDeflateLimit && !buf.ensureTwoByteChars()) {
      return false;
    }

    const Utf8Unit* units = pinned.get();
    return buf.append(units, len);
  } else {
    PinnedUnits<char16_t> pinned(cx, this, holder, start, len);
    if (!pinned.get()) {
      return false;
    }
    if (len > SourceDeflateLimit && !buf.ensureTwoByteChars()) {
      return false;
    }

    const char16_t* units = pinned.get();
    return buf.append(units, len);
  }
}

JSLinearString* ScriptSource::functionBodyString(JSContext* cx) {
  MOZ_ASSERT(isFunctionBody());

  size_t start =
      parameterListEnd_ + (sizeof(FunctionConstructorMedialSigils) - 1);
  size_t stop = length() - (sizeof(FunctionConstructorFinalBrace) - 1);
  return substring(cx, start, stop);
}

template <typename Unit>
[[nodiscard]] bool ScriptSource::setUncompressedSourceHelper(
    JSContext* cx, EntryUnits<Unit>&& source, size_t length,
    SourceRetrievable retrievable) {
  auto& cache = cx->runtime()->sharedImmutableStrings();

  auto uniqueChars = SourceTypeTraits<Unit>::toCacheable(std::move(source));
  auto deduped = cache.getOrCreate(std::move(uniqueChars), length);
  if (!deduped) {
    ReportOutOfMemory(cx);
    return false;
  }

  if (retrievable == SourceRetrievable::Yes) {
    data = SourceType(
        Uncompressed<Unit, SourceRetrievable::Yes>(std::move(deduped)));
  } else {
    data = SourceType(
        Uncompressed<Unit, SourceRetrievable::No>(std::move(deduped)));
  }
  return true;
}

template <typename Unit>
[[nodiscard]] bool ScriptSource::setRetrievedSource(JSContext* cx,
                                                    EntryUnits<Unit>&& source,
                                                    size_t length) {
  MOZ_ASSERT(data.is<Retrievable<Unit>>(),
             "retrieved source can only overwrite the corresponding "
             "retrievable source");
  return setUncompressedSourceHelper(cx, std::move(source), length,
                                     SourceRetrievable::Yes);
}

bool js::IsOffThreadSourceCompressionEnabled() {
  // If we don't have concurrent execution compression will contend with
  // main-thread execution, in which case we disable. Similarly we don't want to
  // block the thread pool if it is too small.
  return GetHelperThreadCPUCount() > 1 && GetHelperThreadCount() > 1 &&
         CanUseExtraThreads();
}

bool ScriptSource::tryCompressOffThread(JSContext* cx) {
  // Beware: |js::SynchronouslyCompressSource| assumes that this function is
  // only called once, just after a script has been compiled, and it's never
  // called at some random time after that.  If multiple calls of this can ever
  // occur, that function may require changes.

  // The SourceCompressionTask needs to record the major GC number for
  // scheduling. This cannot be accessed off-thread and must be handle in
  // ParseTask::finish instead.
  MOZ_ASSERT(!cx->isHelperThreadContext());
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));

  // If source compression was already attempted, do not queue a new task.
  if (hadCompressionTask_) {
    return true;
  }

  if (!hasUncompressedSource()) {
    // This excludes compressed, missing, and retrievable source.
    return true;
  }

  // There are several cases where source compression is not a good idea:
  //  - If the script is tiny, then compression will save little or no space.
  //  - If there is only one core, then compression will contend with JS
  //    execution (which hurts benchmarketing).
  //
  // Otherwise, enqueue a compression task to be processed when a major
  // GC is requested.

  if (length() < ScriptSource::MinimumCompressibleLength ||
      !IsOffThreadSourceCompressionEnabled()) {
    return true;
  }

  // Heap allocate the task. It will be freed upon compression
  // completing in AttachFinishedCompressedSources.
  auto task = MakeUnique<SourceCompressionTask>(cx->runtime(), this);
  if (!task) {
    ReportOutOfMemory(cx);
    return false;
  }
  return EnqueueOffThreadCompression(cx, std::move(task));
}

template <typename Unit>
void ScriptSource::triggerConvertToCompressedSource(
    SharedImmutableString compressed, size_t uncompressedLength) {
  MOZ_ASSERT(isUncompressed<Unit>(),
             "should only be triggering compressed source installation to "
             "overwrite identically-encoded uncompressed source");
  MOZ_ASSERT(uncompressedData<Unit>()->length() == uncompressedLength);

  // If units aren't pinned -- and they probably won't be, we'd have to have a
  // GC in the small window of time where a |PinnedUnits| was live -- then we
  // can immediately convert.
  if (MOZ_LIKELY(!pinnedUnitsStack_)) {
    convertToCompressedSource<Unit>(std::move(compressed), uncompressedLength);
    return;
  }

  // Otherwise, set aside the compressed-data info.  The conversion is performed
  // when the last |PinnedUnits| dies.
  MOZ_ASSERT(pendingCompressed_.empty(),
             "shouldn't be multiple conversions happening");
  pendingCompressed_.construct<CompressedData<Unit>>(std::move(compressed),
                                                     uncompressedLength);
}

template <typename Unit>
[[nodiscard]] bool ScriptSource::initializeWithUnretrievableCompressedSource(
    JSContext* cx, UniqueChars&& compressed, size_t rawLength,
    size_t sourceLength) {
  MOZ_ASSERT(data.is<Missing>(), "shouldn't be double-initializing");
  MOZ_ASSERT(compressed != nullptr);

  auto& cache = cx->runtime()->sharedImmutableStrings();
  auto deduped = cache.getOrCreate(std::move(compressed), rawLength);
  if (!deduped) {
    ReportOutOfMemory(cx);
    return false;
  }

  MOZ_ASSERT(pinnedUnitsStack_ == nullptr,
             "shouldn't be initializing a ScriptSource while its characters "
             "are pinned -- that only makes sense with a ScriptSource actively "
             "being inspected");

  data = SourceType(Compressed<Unit, SourceRetrievable::No>(std::move(deduped),
                                                            sourceLength));

  return true;
}

template <typename Unit>
bool ScriptSource::assignSource(JSContext* cx,
                                const ReadOnlyCompileOptions& options,
                                SourceText<Unit>& srcBuf) {
  MOZ_ASSERT(data.is<Missing>(),
             "source assignment should only occur on fresh ScriptSources");

  if (options.discardSource) {
    return true;
  }

  if (options.sourceIsLazy) {
    data = SourceType(Retrievable<Unit>());
    return true;
  }

  JSRuntime* runtime = cx->runtime();
  auto& cache = runtime->sharedImmutableStrings();
  auto deduped = cache.getOrCreate(srcBuf.get(), srcBuf.length(), [&srcBuf]() {
    using CharT = typename SourceTypeTraits<Unit>::CharT;
    return srcBuf.ownsUnits()
               ? UniquePtr<CharT[], JS::FreePolicy>(srcBuf.takeChars())
               : DuplicateString(srcBuf.get(), srcBuf.length());
  });
  if (!deduped) {
    ReportOutOfMemory(cx);
    return false;
  }

  data =
      SourceType(Uncompressed<Unit, SourceRetrievable::No>(std::move(deduped)));
  return true;
}

template bool ScriptSource::assignSource(JSContext* cx,
                                         const ReadOnlyCompileOptions& options,
                                         SourceText<char16_t>& srcBuf);
template bool ScriptSource::assignSource(JSContext* cx,
                                         const ReadOnlyCompileOptions& options,
                                         SourceText<Utf8Unit>& srcBuf);

void ScriptSource::finalizeGCData() {
  // This should be kept in sync with ScriptSource::trace above.

  // When the canonical ScriptSourceObject's finalizer runs, this
  // ScriptSource can no longer be accessed from the main
  // thread. However, an offthread source compression task may still
  // hold a reference. We must clean up any GC pointers owned by this
  // ScriptSource now, because trying to run those prebarriers
  // offthread later will fail.
  MOZ_ASSERT(TlsContext.get() && TlsContext.get()->isMainThreadContext());

  if (xdrEncoder_) {
    xdrEncoder_.reset();
  }
}

ScriptSource::~ScriptSource() {
  MOZ_ASSERT(refs == 0);

  // GC pointers must have been cleared earlier, because this destructor could
  // be called off-thread by SweepCompressionTasks. See above.
  MOZ_ASSERT(!xdrEncoder_);
}

[[nodiscard]] static bool reallocUniquePtr(UniqueChars& unique, size_t size) {
  auto newPtr = static_cast<char*>(js_realloc(unique.get(), size));
  if (!newPtr) {
    return false;
  }

  // Since the realloc succeeded, unique is now holding a freed pointer.
  (void)unique.release();
  unique.reset(newPtr);
  return true;
}

template <typename Unit>
void SourceCompressionTask::workEncodingSpecific() {
  MOZ_ASSERT(source_->isUncompressed<Unit>());

  // Try to keep the maximum memory usage down by only allocating half the
  // size of the string, first.
  size_t inputBytes = source_->length() * sizeof(Unit);
  size_t firstSize = inputBytes / 2;
  UniqueChars compressed(js_pod_malloc<char>(firstSize));
  if (!compressed) {
    return;
  }

  const Unit* chars = source_->uncompressedData<Unit>()->units();
  Compressor comp(reinterpret_cast<const unsigned char*>(chars), inputBytes);
  if (!comp.init()) {
    return;
  }

  comp.setOutput(reinterpret_cast<unsigned char*>(compressed.get()), firstSize);
  bool cont = true;
  bool reallocated = false;
  while (cont) {
    if (shouldCancel()) {
      return;
    }

    switch (comp.compressMore()) {
      case Compressor::CONTINUE:
        break;
      case Compressor::MOREOUTPUT: {
        if (reallocated) {
          // The compressed string is longer than the original string.
          return;
        }

        // The compressed output is greater than half the size of the
        // original string. Reallocate to the full size.
        if (!reallocUniquePtr(compressed, inputBytes)) {
          return;
        }

        comp.setOutput(reinterpret_cast<unsigned char*>(compressed.get()),
                       inputBytes);
        reallocated = true;
        break;
      }
      case Compressor::DONE:
        cont = false;
        break;
      case Compressor::OOM:
        return;
    }
  }

  size_t totalBytes = comp.totalBytesNeeded();

  // Shrink the buffer to the size of the compressed data.
  if (!reallocUniquePtr(compressed, totalBytes)) {
    return;
  }

  comp.finish(compressed.get(), totalBytes);

  if (shouldCancel()) {
    return;
  }

  auto& strings = runtime_->sharedImmutableStrings();
  resultString_ = strings.getOrCreate(std::move(compressed), totalBytes);
}

struct SourceCompressionTask::PerformTaskWork {
  SourceCompressionTask* const task_;

  explicit PerformTaskWork(SourceCompressionTask* task) : task_(task) {}

  template <typename Unit, SourceRetrievable CanRetrieve>
  void operator()(const ScriptSource::Uncompressed<Unit, CanRetrieve>&) {
    task_->workEncodingSpecific<Unit>();
  }

  template <typename T>
  void operator()(const T&) {
    MOZ_CRASH(
        "why are we compressing missing, missing-but-retrievable, "
        "or already-compressed source?");
  }
};

void ScriptSource::performTaskWork(SourceCompressionTask* task) {
  MOZ_ASSERT(hasUncompressedSource());
  data.match(SourceCompressionTask::PerformTaskWork(task));
}

void SourceCompressionTask::runTask() {
  if (shouldCancel()) {
    return;
  }

  TraceLoggerThread* logger = TraceLoggerForCurrentThread();
  AutoTraceLog logCompile(logger, TraceLogger_CompressSource);

  MOZ_ASSERT(source_->hasUncompressedSource());

  source_->performTaskWork(this);
}

void SourceCompressionTask::runHelperThreadTask(
    AutoLockHelperThreadState& locked) {
  {
    AutoUnlockHelperThreadState unlock(locked);
    this->runTask();
  }

  {
    AutoEnterOOMUnsafeRegion oomUnsafe;
    if (!HelperThreadState().compressionFinishedList(locked).append(this)) {
      oomUnsafe.crash("SourceCompressionTask::runHelperThreadTask");
    }
  }
}

void ScriptSource::triggerConvertToCompressedSourceFromTask(
    SharedImmutableString compressed) {
  data.match(TriggerConvertToCompressedSourceFromTask(this, compressed));
}

void SourceCompressionTask::complete() {
  if (!shouldCancel() && resultString_) {
    source_->triggerConvertToCompressedSourceFromTask(std::move(resultString_));
  }
}

bool js::SynchronouslyCompressSource(JSContext* cx,
                                     JS::Handle<BaseScript*> script) {
  MOZ_ASSERT(!cx->isHelperThreadContext(),
             "should only sync-compress on the main thread");

  // Finish all pending source compressions, including the single compression
  // task that may have been created (by |ScriptSource::tryCompressOffThread|)
  // just after the script was compiled.  Because we have flushed this queue,
  // no code below needs to synchronize with an off-thread parse task that
  // assumes the immutability of a |ScriptSource|'s data.
  //
  // This *may* end up compressing |script|'s source.  If it does -- we test
  // this below -- that takes care of things.  But if it doesn't, we will
  // synchronously compress ourselves (and as noted above, this won't race
  // anything).
  RunPendingSourceCompressions(cx->runtime());

  ScriptSource* ss = script->scriptSource();
  MOZ_ASSERT(!ss->pinnedUnitsStack_,
             "can't synchronously compress while source units are in use");

  // In principle a previously-triggered compression on a helper thread could
  // have already completed.  If that happens, there's nothing more to do.
  if (ss->hasCompressedSource()) {
    return true;
  }

  MOZ_ASSERT(ss->hasUncompressedSource(),
             "shouldn't be compressing uncompressible source");

  // Use an explicit scope to delineate the lifetime of |task|, for simplicity.
  {
#ifdef DEBUG
    uint32_t sourceRefs = ss->refs;
#endif
    MOZ_ASSERT(sourceRefs > 0, "at least |script| here should have a ref");

    // |SourceCompressionTask::shouldCancel| can periodically result in source
    // compression being canceled if we're not careful.  Guarantee that two refs
    // to |ss| are always live in this function (at least one preexisting and
    // one held by the task) so that compression is never canceled.
    auto task = MakeUnique<SourceCompressionTask>(cx->runtime(), ss);
    if (!task) {
      ReportOutOfMemory(cx);
      return false;
    }

    MOZ_ASSERT(ss->refs > sourceRefs, "must have at least two refs now");

    // Attempt to compress.  This may not succeed if OOM happens, but (because
    // it ordinarily happens on a helper thread) no error will ever be set here.
    MOZ_ASSERT(!cx->isExceptionPending());
    ss->performTaskWork(task.get());
    MOZ_ASSERT(!cx->isExceptionPending());

    // Convert |ss| from uncompressed to compressed data.
    task->complete();

    MOZ_ASSERT(!cx->isExceptionPending());
  }

  // The only way source won't be compressed here is if OOM happened.
  return ss->hasCompressedSource();
}

void ScriptSource::addSizeOfIncludingThis(mozilla::MallocSizeOf mallocSizeOf,
                                          JS::ScriptSourceInfo* info) const {
  info->misc += mallocSizeOf(this);
  info->numScripts++;
}

bool ScriptSource::startIncrementalEncoding(
    JSContext* cx, const JS::ReadOnlyCompileOptions& options,
    UniquePtr<frontend::ExtensibleCompilationStencil>&& initial) {
  // Encoding failures are reported by the xdrFinalizeEncoder function.
  if (containsAsmJS()) {
    return true;
  }

  // Remove the reference to the source, to avoid the circular reference.
  initial->source = nullptr;

  xdrEncoder_ = js::MakeUnique<XDRIncrementalStencilEncoder>();
  if (!xdrEncoder_) {
    ReportOutOfMemory(cx);
    return false;
  }

  AutoIncrementalTimer timer(cx->realm()->timers.xdrEncodingTime);
  auto failureCase =
      mozilla::MakeScopeExit([&] { xdrEncoder_.reset(nullptr); });

  XDRResult res = xdrEncoder_->setInitial(
      cx, options,
      std::forward<UniquePtr<frontend::ExtensibleCompilationStencil>>(initial));
  if (res.isErr()) {
    // On encoding failure, let failureCase destroy encoder and return true
    // to avoid failing any currently executing script.
    return JS::IsTranscodeFailureResult(res.unwrapErr());
  }

  failureCase.release();
  return true;
}

bool ScriptSource::addDelazificationToIncrementalEncoding(
    JSContext* cx, const frontend::CompilationStencil& stencil) {
  MOZ_ASSERT(hasEncoder());
  AutoIncrementalTimer timer(cx->realm()->timers.xdrEncodingTime);
  auto failureCase =
      mozilla::MakeScopeExit([&] { xdrEncoder_.reset(nullptr); });

  XDRResult res = xdrEncoder_->addDelazification(cx, stencil);
  if (res.isErr()) {
    // On encoding failure, let failureCase destroy encoder and return true
    // to avoid failing any currently executing script.
    return JS::IsTranscodeFailureResult(res.unwrapErr());
  }

  failureCase.release();
  return true;
}

bool ScriptSource::xdrFinalizeEncoder(JSContext* cx,
                                      JS::TranscodeBuffer& buffer) {
  if (!hasEncoder()) {
    JS_ReportErrorASCII(cx, "XDR encoding failure");
    return false;
  }

  auto cleanup = mozilla::MakeScopeExit([&] { xdrEncoder_.reset(nullptr); });

  XDRResult res = xdrEncoder_->linearize(cx, buffer, this);
  if (res.isErr()) {
    if (JS::IsTranscodeFailureResult(res.unwrapErr())) {
      JS_ReportErrorASCII(cx, "XDR encoding failure");
    }
    return false;
  }
  return true;
}

template <typename Unit>
[[nodiscard]] bool ScriptSource::initializeUnretrievableUncompressedSource(
    JSContext* cx, EntryUnits<Unit>&& source, size_t length) {
  MOZ_ASSERT(data.is<Missing>(), "must be initializing a fresh ScriptSource");
  return setUncompressedSourceHelper(cx, std::move(source), length,
                                     SourceRetrievable::No);
}

template <typename Unit>
struct UnretrievableSourceDecoder {
  XDRState<XDR_DECODE>* const xdr_;
  ScriptSource* const scriptSource_;
  const uint32_t uncompressedLength_;

 public:
  UnretrievableSourceDecoder(XDRState<XDR_DECODE>* xdr,
                             ScriptSource* scriptSource,
                             uint32_t uncompressedLength)
      : xdr_(xdr),
        scriptSource_(scriptSource),
        uncompressedLength_(uncompressedLength) {}

  XDRResult decode() {
    auto sourceUnits = xdr_->cx()->make_pod_array<Unit>(
        std::max<size_t>(uncompressedLength_, 1));
    if (!sourceUnits) {
      return xdr_->fail(JS::TranscodeResult::Throw);
    }

    MOZ_TRY(xdr_->codeChars(sourceUnits.get(), uncompressedLength_));

    if (!scriptSource_->initializeUnretrievableUncompressedSource(
            xdr_->cx(), std::move(sourceUnits), uncompressedLength_)) {
      return xdr_->fail(JS::TranscodeResult::Throw);
    }

    return Ok();
  }
};

namespace js {

template <>
XDRResult ScriptSource::xdrUnretrievableUncompressedSource<XDR_DECODE>(
    XDRState<XDR_DECODE>* xdr, uint8_t sourceCharSize,
    uint32_t uncompressedLength) {
  MOZ_ASSERT(sourceCharSize == 1 || sourceCharSize == 2);

  if (sourceCharSize == 1) {
    UnretrievableSourceDecoder<Utf8Unit> decoder(xdr, this, uncompressedLength);
    return decoder.decode();
  }

  UnretrievableSourceDecoder<char16_t> decoder(xdr, this, uncompressedLength);
  return decoder.decode();
}

}  // namespace js

template <typename Unit>
struct UnretrievableSourceEncoder {
  XDRState<XDR_ENCODE>* const xdr_;
  ScriptSource* const source_;
  const uint32_t uncompressedLength_;

  UnretrievableSourceEncoder(XDRState<XDR_ENCODE>* xdr, ScriptSource* source,
                             uint32_t uncompressedLength)
      : xdr_(xdr), source_(source), uncompressedLength_(uncompressedLength) {}

  XDRResult encode() {
    Unit* sourceUnits =
        const_cast<Unit*>(source_->uncompressedData<Unit>()->units());

    return xdr_->codeChars(sourceUnits, uncompressedLength_);
  }
};

namespace js {

template <>
XDRResult ScriptSource::xdrUnretrievableUncompressedSource<XDR_ENCODE>(
    XDRState<XDR_ENCODE>* xdr, uint8_t sourceCharSize,
    uint32_t uncompressedLength) {
  MOZ_ASSERT(sourceCharSize == 1 || sourceCharSize == 2);

  if (sourceCharSize == 1) {
    UnretrievableSourceEncoder<Utf8Unit> encoder(xdr, this, uncompressedLength);
    return encoder.encode();
  }

  UnretrievableSourceEncoder<char16_t> encoder(xdr, this, uncompressedLength);
  return encoder.encode();
}

}  // namespace js

template <typename Unit, XDRMode mode>
/* static */
XDRResult ScriptSource::codeUncompressedData(XDRState<mode>* const xdr,
                                             ScriptSource* const ss) {
  static_assert(
      std::is_same_v<Unit, Utf8Unit> || std::is_same_v<Unit, char16_t>,
      "should handle UTF-8 and UTF-16");

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(ss->isUncompressed<Unit>());
  } else {
    MOZ_ASSERT(ss->data.is<Missing>());
  }

  uint32_t uncompressedLength;
  if (mode == XDR_ENCODE) {
    uncompressedLength = ss->uncompressedData<Unit>()->length();
  }
  MOZ_TRY(xdr->codeUint32(&uncompressedLength));

  return ss->xdrUnretrievableUncompressedSource(xdr, sizeof(Unit),
                                                uncompressedLength);
}

template <typename Unit, XDRMode mode>
/* static */
XDRResult ScriptSource::codeCompressedData(XDRState<mode>* const xdr,
                                           ScriptSource* const ss) {
  static_assert(
      std::is_same_v<Unit, Utf8Unit> || std::is_same_v<Unit, char16_t>,
      "should handle UTF-8 and UTF-16");

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(ss->isCompressed<Unit>());
  } else {
    MOZ_ASSERT(ss->data.is<Missing>());
  }

  uint32_t uncompressedLength;
  if (mode == XDR_ENCODE) {
    uncompressedLength = ss->data.as<Compressed<Unit, SourceRetrievable::No>>()
                             .uncompressedLength;
  }
  MOZ_TRY(xdr->codeUint32(&uncompressedLength));

  uint32_t compressedLength;
  if (mode == XDR_ENCODE) {
    compressedLength =
        ss->data.as<Compressed<Unit, SourceRetrievable::No>>().raw.length();
  }
  MOZ_TRY(xdr->codeUint32(&compressedLength));

  if (mode == XDR_DECODE) {
    // Compressed data is always single-byte chars.
    auto bytes = xdr->cx()->template make_pod_array<char>(compressedLength);
    if (!bytes) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
    MOZ_TRY(xdr->codeBytes(bytes.get(), compressedLength));

    if (!ss->initializeWithUnretrievableCompressedSource<Unit>(
            xdr->cx(), std::move(bytes), compressedLength,
            uncompressedLength)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  } else {
    void* bytes = const_cast<char*>(ss->compressedData<Unit>()->raw.chars());
    MOZ_TRY(xdr->codeBytes(bytes, compressedLength));
  }

  return Ok();
}

template <typename Unit,
          template <typename U, SourceRetrievable CanRetrieve> class Data,
          XDRMode mode>
/* static */
void ScriptSource::codeRetrievable(ScriptSource* const ss) {
  static_assert(
      std::is_same_v<Unit, Utf8Unit> || std::is_same_v<Unit, char16_t>,
      "should handle UTF-8 and UTF-16");

  if (mode == XDR_ENCODE) {
    MOZ_ASSERT((ss->data.is<Data<Unit, SourceRetrievable::Yes>>()));
  } else {
    MOZ_ASSERT(ss->data.is<Missing>());
    ss->data = SourceType(Retrievable<Unit>());
  }
}

template <typename Unit, XDRMode mode>
/* static */
void ScriptSource::codeRetrievableData(ScriptSource* ss) {
  // There's nothing to code for retrievable data.  Just be sure to set
  // retrievable data when decoding.
  if (mode == XDR_ENCODE) {
    MOZ_ASSERT(ss->data.is<Retrievable<Unit>>());
  } else {
    MOZ_ASSERT(ss->data.is<Missing>());
    ss->data = SourceType(Retrievable<Unit>());
  }
}

template <XDRMode mode>
/* static */
XDRResult ScriptSource::xdrData(XDRState<mode>* const xdr,
                                ScriptSource* const ss) {
  // The order here corresponds to the type order in |ScriptSource::SourceType|
  // so number->internal Variant tag is a no-op.
  enum class DataType {
    CompressedUtf8Retrievable,
    UncompressedUtf8Retrievable,
    CompressedUtf8NotRetrievable,
    UncompressedUtf8NotRetrievable,
    CompressedUtf16Retrievable,
    UncompressedUtf16Retrievable,
    CompressedUtf16NotRetrievable,
    UncompressedUtf16NotRetrievable,
    RetrievableUtf8,
    RetrievableUtf16,
    Missing,
  };

  DataType tag;
  {
    // This is terrible, but we can't do better.  When |mode == XDR_DECODE| we
    // don't have a |ScriptSource::data| |Variant| to match -- the entire XDR
    // idiom for tagged unions depends on coding a tag-number, then the
    // corresponding tagged data.  So we must manually define a tag-enum, code
    // it, then switch on it (and ignore the |Variant::match| API).
    class XDRDataTag {
     public:
      DataType operator()(const Compressed<Utf8Unit, SourceRetrievable::Yes>&) {
        return DataType::CompressedUtf8Retrievable;
      }
      DataType operator()(
          const Uncompressed<Utf8Unit, SourceRetrievable::Yes>&) {
        return DataType::UncompressedUtf8Retrievable;
      }
      DataType operator()(const Compressed<Utf8Unit, SourceRetrievable::No>&) {
        return DataType::CompressedUtf8NotRetrievable;
      }
      DataType operator()(
          const Uncompressed<Utf8Unit, SourceRetrievable::No>&) {
        return DataType::UncompressedUtf8NotRetrievable;
      }
      DataType operator()(const Compressed<char16_t, SourceRetrievable::Yes>&) {
        return DataType::CompressedUtf16Retrievable;
      }
      DataType operator()(
          const Uncompressed<char16_t, SourceRetrievable::Yes>&) {
        return DataType::UncompressedUtf16Retrievable;
      }
      DataType operator()(const Compressed<char16_t, SourceRetrievable::No>&) {
        return DataType::CompressedUtf16NotRetrievable;
      }
      DataType operator()(
          const Uncompressed<char16_t, SourceRetrievable::No>&) {
        return DataType::UncompressedUtf16NotRetrievable;
      }
      DataType operator()(const Retrievable<Utf8Unit>&) {
        return DataType::RetrievableUtf8;
      }
      DataType operator()(const Retrievable<char16_t>&) {
        return DataType::RetrievableUtf16;
      }
      DataType operator()(const Missing&) { return DataType::Missing; }
    };

    uint8_t type;
    if (mode == XDR_ENCODE) {
      type = static_cast<uint8_t>(ss->data.match(XDRDataTag()));
    }
    MOZ_TRY(xdr->codeUint8(&type));

    if (type > static_cast<uint8_t>(DataType::Missing)) {
      // Fail in debug, but only soft-fail in release, if the type is invalid.
      MOZ_ASSERT_UNREACHABLE("bad tag");
      return xdr->fail(JS::TranscodeResult::Failure_BadDecode);
    }

    tag = static_cast<DataType>(type);
  }

  switch (tag) {
    case DataType::CompressedUtf8Retrievable:
      ScriptSource::codeRetrievable<Utf8Unit, Compressed, mode>(ss);
      return Ok();

    case DataType::CompressedUtf8NotRetrievable:
      return ScriptSource::codeCompressedData<Utf8Unit>(xdr, ss);

    case DataType::UncompressedUtf8Retrievable:
      ScriptSource::codeRetrievable<Utf8Unit, Uncompressed, mode>(ss);
      return Ok();

    case DataType::UncompressedUtf8NotRetrievable:
      return ScriptSource::codeUncompressedData<Utf8Unit>(xdr, ss);

    case DataType::CompressedUtf16Retrievable:
      ScriptSource::codeRetrievable<char16_t, Compressed, mode>(ss);
      return Ok();

    case DataType::CompressedUtf16NotRetrievable:
      return ScriptSource::codeCompressedData<char16_t>(xdr, ss);

    case DataType::UncompressedUtf16Retrievable:
      ScriptSource::codeRetrievable<char16_t, Uncompressed, mode>(ss);
      return Ok();

    case DataType::UncompressedUtf16NotRetrievable:
      return ScriptSource::codeUncompressedData<char16_t>(xdr, ss);

    case DataType::Missing: {
      MOZ_ASSERT(ss->data.is<Missing>(),
                 "ScriptSource::data is initialized as missing, so neither "
                 "encoding nor decoding has to change anything");

      // There's no data to XDR for missing source.
      break;
    }

    case DataType::RetrievableUtf8:
      ScriptSource::codeRetrievableData<Utf8Unit, mode>(ss);
      return Ok();

    case DataType::RetrievableUtf16:
      ScriptSource::codeRetrievableData<char16_t, mode>(ss);
      return Ok();
  }

  // The range-check on |type| far above ought ensure the above |switch| is
  // exhaustive and all cases will return, but not all compilers understand
  // this.  Make the Missing case break to here so control obviously never flows
  // off the end.
  MOZ_ASSERT(tag == DataType::Missing);
  return Ok();
}

template <XDRMode mode>
/* static */
XDRResult ScriptSource::XDR(XDRState<mode>* xdr,
                            const ReadOnlyCompileOptions* maybeOptions,
                            RefPtr<ScriptSource>& source) {
  JSContext* cx = xdr->cx();

  if (mode == XDR_DECODE) {
    // Allocate a new ScriptSource and root it with the holder.
    source = do_AddRef(cx->new_<ScriptSource>());
    if (!source) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }

    // We use this CompileOptions only to initialize the ScriptSourceObject.
    // Most CompileOptions fields aren't used by ScriptSourceObject, and those
    // that are (element; elementAttributeName) aren't preserved by XDR. So
    // this can be simple.
    if (!source->initFromOptions(cx, *maybeOptions)) {
      return xdr->fail(JS::TranscodeResult::Throw);
    }
  }

  MOZ_TRY(xdrData(xdr, source.get()));

  uint8_t haveSourceMap = source->hasSourceMapURL();
  MOZ_TRY(xdr->codeUint8(&haveSourceMap));

  if (haveSourceMap) {
    XDRTranscodeString<char16_t> chars;

    if (mode == XDR_ENCODE) {
      chars.construct<const char16_t*>(source->sourceMapURL());
    }
    MOZ_TRY(xdr->codeCharsZ(chars));
    if (mode == XDR_DECODE) {
      if (!source->setSourceMapURL(
              cx, std::move(chars.ref<UniqueTwoByteChars>()))) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  uint8_t haveDisplayURL = source->hasDisplayURL();
  MOZ_TRY(xdr->codeUint8(&haveDisplayURL));

  if (haveDisplayURL) {
    XDRTranscodeString<char16_t> chars;

    if (mode == XDR_ENCODE) {
      chars.construct<const char16_t*>(source->displayURL());
    }
    MOZ_TRY(xdr->codeCharsZ(chars));
    if (mode == XDR_DECODE) {
      if (!source->setDisplayURL(cx,
                                 std::move(chars.ref<UniqueTwoByteChars>()))) {
        return xdr->fail(JS::TranscodeResult::Throw);
      }
    }
  }

  uint8_t haveFilename = !!source->filename_;
  MOZ_TRY(xdr->codeUint8(&haveFilename));

  if (haveFilename) {
    XDRTranscodeString<char> chars;

    if (mode == XDR_ENCODE) {
      chars.construct<const char*>(source->filename());
    }
    MOZ_TRY(xdr->codeCharsZ(chars));
    if (mode == XDR_DECODE) {
      if (!source->filename()) {
        if (!source->setFilename(cx, std::move(chars.ref<UniqueChars>()))) {
          return xdr->fail(JS::TranscodeResult::Throw);
        }
      }
      MOZ_ASSERT(source->filename());
    }
  }

  return Ok();
}

template /* static */
    XDRResult
    ScriptSource::XDR(XDRState<XDR_ENCODE>* xdr,
                      const ReadOnlyCompileOptions* maybeOptions,
                      RefPtr<ScriptSource>& holder);
template /* static */
    XDRResult
    ScriptSource::XDR(XDRState<XDR_DECODE>* xdr,
                      const ReadOnlyCompileOptions* maybeOptions,
                      RefPtr<ScriptSource>& holder);

// Format and return a cx->pod_malloc'ed URL for a generated script like:
//   {filename} line {lineno} > {introducer}
// For example:
//   foo.js line 7 > eval
// indicating code compiled by the call to 'eval' on line 7 of foo.js.
UniqueChars js::FormatIntroducedFilename(JSContext* cx, const char* filename,
                                         unsigned lineno,
                                         const char* introducer) {
  // Compute the length of the string in advance, so we can allocate a
  // buffer of the right size on the first shot.
  //
  // (JS_smprintf would be perfect, as that allocates the result
  // dynamically as it formats the string, but it won't allocate from cx,
  // and wants us to use a special free function.)
  char linenoBuf[15];
  size_t filenameLen = strlen(filename);
  size_t linenoLen = SprintfLiteral(linenoBuf, "%u", lineno);
  size_t introducerLen = strlen(introducer);
  size_t len = filenameLen + 6 /* == strlen(" line ") */ + linenoLen +
               3 /* == strlen(" > ") */ + introducerLen + 1 /* \0 */;
  UniqueChars formatted(cx->pod_malloc<char>(len));
  if (!formatted) {
    return nullptr;
  }

  mozilla::DebugOnly<size_t> checkLen = snprintf(
      formatted.get(), len, "%s line %s > %s", filename, linenoBuf, introducer);
  MOZ_ASSERT(checkLen == len - 1);

  return formatted;
}

bool ScriptSource::initFromOptions(JSContext* cx,
                                   const ReadOnlyCompileOptions& options) {
  MOZ_ASSERT(!filename_);
  MOZ_ASSERT(!introducerFilename_);

  mutedErrors_ = options.mutedErrors();

  startLine_ = options.lineno;
  introductionType_ = options.introductionType;
  setIntroductionOffset(options.introductionOffset);
  // The parameterListEnd_ is initialized later by setParameterListEnd, before
  // we expose any scripts that use this ScriptSource to the debugger.

  if (options.hasIntroductionInfo) {
    MOZ_ASSERT(options.introductionType != nullptr);
    const char* filename =
        options.filename() ? options.filename() : "<unknown>";
    UniqueChars formatted = FormatIntroducedFilename(
        cx, filename, options.introductionLineno, options.introductionType);
    if (!formatted) {
      return false;
    }
    if (!setFilename(cx, std::move(formatted))) {
      return false;
    }
  } else if (options.filename()) {
    if (!setFilename(cx, options.filename())) {
      return false;
    }
  }

  if (options.introducerFilename()) {
    if (!setIntroducerFilename(cx, options.introducerFilename())) {
      return false;
    }
  }

  return true;
}

// Use the SharedImmutableString map to deduplicate input string. The input
// string must be null-terminated.
template <typename SharedT, typename CharT>
static SharedT GetOrCreateStringZ(JSContext* cx,
                                  UniquePtr<CharT[], JS::FreePolicy>&& str) {
  JSRuntime* rt = cx->runtime();
  size_t lengthWithNull = std::char_traits<CharT>::length(str.get()) + 1;
  auto res =
      rt->sharedImmutableStrings().getOrCreate(std::move(str), lengthWithNull);
  if (!res) {
    ReportOutOfMemory(cx);
  }
  return res;
}

SharedImmutableString ScriptSource::getOrCreateStringZ(JSContext* cx,
                                                       UniqueChars&& str) {
  return GetOrCreateStringZ<SharedImmutableString>(cx, std::move(str));
}

SharedImmutableTwoByteString ScriptSource::getOrCreateStringZ(
    JSContext* cx, UniqueTwoByteChars&& str) {
  return GetOrCreateStringZ<SharedImmutableTwoByteString>(cx, std::move(str));
}

bool ScriptSource::setFilename(JSContext* cx, const char* filename) {
  UniqueChars owned = DuplicateString(cx, filename);
  if (!owned) {
    return false;
  }
  return setFilename(cx, std::move(owned));
}

bool ScriptSource::setFilename(JSContext* cx, UniqueChars&& filename) {
  MOZ_ASSERT(!filename_);
  filename_ = getOrCreateStringZ(cx, std::move(filename));
  return bool(filename_);
}

bool ScriptSource::setIntroducerFilename(JSContext* cx, const char* filename) {
  UniqueChars owned = DuplicateString(cx, filename);
  if (!owned) {
    return false;
  }
  return setIntroducerFilename(cx, std::move(owned));
}

bool ScriptSource::setIntroducerFilename(JSContext* cx,
                                         UniqueChars&& filename) {
  MOZ_ASSERT(!introducerFilename_);
  introducerFilename_ = getOrCreateStringZ(cx, std::move(filename));
  return bool(introducerFilename_);
}

bool ScriptSource::setDisplayURL(JSContext* cx, const char16_t* url) {
  UniqueTwoByteChars owned = DuplicateString(cx, url);
  if (!owned) {
    return false;
  }
  return setDisplayURL(cx, std::move(owned));
}

bool ScriptSource::setDisplayURL(JSContext* cx, UniqueTwoByteChars&& url) {
  if (hasDisplayURL()) {
    // FIXME: filename() should be UTF-8 (bug 987069).
    if (!cx->isHelperThreadContext() &&
        !WarnNumberLatin1(cx, JSMSG_ALREADY_HAS_PRAGMA, filename(),
                          "//# sourceURL")) {
      return false;
    }
  }

  MOZ_ASSERT(url);
  if (url[0] == '\0') {
    return true;
  }

  displayURL_ = getOrCreateStringZ(cx, std::move(url));
  return bool(displayURL_);
}

bool ScriptSource::setSourceMapURL(JSContext* cx, const char16_t* url) {
  UniqueTwoByteChars owned = DuplicateString(cx, url);
  if (!owned) {
    return false;
  }
  return setSourceMapURL(cx, std::move(owned));
}

bool ScriptSource::setSourceMapURL(JSContext* cx, UniqueTwoByteChars&& url) {
  MOZ_ASSERT(url);
  if (url[0] == '\0') {
    return true;
  }

  sourceMapURL_ = getOrCreateStringZ(cx, std::move(url));
  return bool(sourceMapURL_);
}

/* static */ mozilla::Atomic<uint32_t, mozilla::SequentiallyConsistent>
    ScriptSource::idCount_;

/*
 * [SMDOC] JSScript data layout (immutable)
 *
 * Script data that shareable across processes. There are no pointers (GC or
 * otherwise) and the data is relocatable.
 *
 * Array elements   Pointed to by         Length
 * --------------   -------------         ------
 * jsbytecode       code()                codeLength()
 * jsscrnote        notes()               noteLength()
 * uint32_t         resumeOffsets()
 * ScopeNote        scopeNotes()
 * TryNote          tryNotes()
 */

/* static */ CheckedInt<uint32_t> ImmutableScriptData::sizeFor(
    uint32_t codeLength, uint32_t noteLength, uint32_t numResumeOffsets,
    uint32_t numScopeNotes, uint32_t numTryNotes) {
  // Take a count of which optional arrays will be used and need offset info.
  unsigned numOptionalArrays = unsigned(numResumeOffsets > 0) +
                               unsigned(numScopeNotes > 0) +
                               unsigned(numTryNotes > 0);

  // Compute size including trailing arrays.
  CheckedInt<uint32_t> size = sizeof(ImmutableScriptData);
  size += sizeof(Flags);
  size += CheckedInt<uint32_t>(codeLength) * sizeof(jsbytecode);
  size += CheckedInt<uint32_t>(noteLength) * sizeof(SrcNote);
  size += CheckedInt<uint32_t>(numOptionalArrays) * sizeof(Offset);
  size += CheckedInt<uint32_t>(numResumeOffsets) * sizeof(uint32_t);
  size += CheckedInt<uint32_t>(numScopeNotes) * sizeof(ScopeNote);
  size += CheckedInt<uint32_t>(numTryNotes) * sizeof(TryNote);

  return size;
}

js::UniquePtr<ImmutableScriptData> js::ImmutableScriptData::new_(
    JSContext* cx, uint32_t codeLength, uint32_t noteLength,
    uint32_t numResumeOffsets, uint32_t numScopeNotes, uint32_t numTryNotes) {
  auto size = sizeFor(codeLength, noteLength, numResumeOffsets, numScopeNotes,
                      numTryNotes);
  if (!size.isValid()) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  // Allocate contiguous raw buffer.
  void* raw = cx->pod_malloc<uint8_t>(size.value());
  MOZ_ASSERT(uintptr_t(raw) % alignof(ImmutableScriptData) == 0);
  if (!raw) {
    return nullptr;
  }

  // Constuct the ImmutableScriptData. Trailing arrays are uninitialized but
  // GCPtrs are put into a safe state.
  UniquePtr<ImmutableScriptData> result(new (raw) ImmutableScriptData(
      codeLength, noteLength, numResumeOffsets, numScopeNotes, numTryNotes));
  if (!result) {
    return nullptr;
  }

  // Sanity check
  MOZ_ASSERT(result->endOffset() == size.value());

  return result;
}

js::UniquePtr<ImmutableScriptData> js::ImmutableScriptData::new_(
    JSContext* cx, uint32_t totalSize) {
  void* raw = cx->pod_malloc<uint8_t>(totalSize);
  MOZ_ASSERT(uintptr_t(raw) % alignof(ImmutableScriptData) == 0);
  UniquePtr<ImmutableScriptData> result(
      reinterpret_cast<ImmutableScriptData*>(raw));
  return result;
}

uint32_t js::ImmutableScriptData::computedSize() {
  auto size = sizeFor(codeLength(), noteLength(), resumeOffsets().size(),
                      scopeNotes().size(), tryNotes().size());
  MOZ_ASSERT(size.isValid());
  return size.value();
}

/* static */
SharedImmutableScriptData* SharedImmutableScriptData::create(JSContext* cx) {
  return cx->new_<SharedImmutableScriptData>();
}

/* static */
SharedImmutableScriptData* SharedImmutableScriptData::createWith(
    JSContext* cx, js::UniquePtr<ImmutableScriptData>&& isd) {
  MOZ_ASSERT(isd.get());
  SharedImmutableScriptData* sisd = create(cx);
  if (!sisd) {
    return nullptr;
  }

  sisd->setOwn(std::move(isd));
  return sisd;
}

void JSScript::relazify(JSRuntime* rt) {
  js::Scope* scope = enclosingScope();
  UniquePtr<PrivateScriptData> scriptData;

#ifndef JS_CODEGEN_NONE
  // Any JIT compiles should have been released, so we already point to the
  // interpreter trampoline which supports lazy scripts.
  MOZ_ASSERT(isUsingInterpreterTrampoline(rt));
#endif

  // Without bytecode, the script counts are invalid so destroy them if they
  // still exist.
  destroyScriptCounts();

  // Release the bytecode and gcthings list.
  // NOTE: We clear the PrivateScriptData to nullptr. This is fine because we
  //       only allowed relazification (via AllowRelazify) if the original lazy
  //       script we compiled from had a nullptr PrivateScriptData.
  swapData(scriptData);
  freeSharedData();

  // We should not still be in any side-tables for the debugger or
  // code-coverage. The finalizer will not be able to clean them up once
  // bytecode is released. We check in JSFunction::maybeRelazify() for these
  // conditions before requesting relazification.
  MOZ_ASSERT(!coverage::IsLCovEnabled());
  MOZ_ASSERT(!hasScriptCounts());
  MOZ_ASSERT(!hasDebugScript());

  // Rollback warmUpData_ to have enclosingScope.
  MOZ_ASSERT(warmUpData_.isWarmUpCount(),
             "JitScript should already be released");
  warmUpData_.resetWarmUpCount(0);
  warmUpData_.initEnclosingScope(scope);

  MOZ_ASSERT(isReadyForDelazification());
}

// Takes ownership of the passed SharedImmutableScriptData and either adds it
// into the runtime's SharedImmutableScriptDataTable, or frees it if a matching
// entry already exists and replaces the passed RefPtr with the existing entry.
/* static */
bool SharedImmutableScriptData::shareScriptData(
    JSContext* cx, RefPtr<SharedImmutableScriptData>& sisd) {
  MOZ_ASSERT(sisd);
  MOZ_ASSERT(sisd->refCount() == 1);

  SharedImmutableScriptData* data = sisd.get();

  // Calculate the hash before taking the lock. Because the data is reference
  // counted, it also will be freed after releasing the lock if necessary.
  SharedImmutableScriptData::Hasher::Lookup lookup(data);

  AutoLockScriptData lock(cx->runtime());

  SharedImmutableScriptDataTable::AddPtr p =
      cx->scriptDataTable(lock).lookupForAdd(lookup);
  if (p) {
    MOZ_ASSERT(data != *p);
    sisd = *p;
  } else {
    if (!cx->scriptDataTable(lock).add(p, data)) {
      ReportOutOfMemory(cx);
      return false;
    }

    // Being in the table counts as a reference on the script data.
    data->AddRef();
  }

  // Refs: sisd argument, SharedImmutableScriptDataTable
  MOZ_ASSERT(sisd->refCount() >= 2);

  return true;
}

void js::SweepScriptData(JSRuntime* rt) {
  // Entries are removed from the table when their reference count is one,
  // i.e. when the only reference to them is from the table entry.

  AutoLockScriptData lock(rt);
  SharedImmutableScriptDataTable& table = rt->scriptDataTable(lock);

  for (SharedImmutableScriptDataTable::Enum e(table); !e.empty();
       e.popFront()) {
    SharedImmutableScriptData* sharedData = e.front();
    if (sharedData->refCount() == 1) {
      sharedData->Release();
      e.removeFront();
    }
  }
}

inline size_t PrivateScriptData::allocationSize() const { return endOffset(); }

// Initialize and placement-new the trailing arrays.
PrivateScriptData::PrivateScriptData(uint32_t ngcthings)
    : ngcthings(ngcthings) {
  // Variable-length data begins immediately after PrivateScriptData itself.
  // NOTE: Alignment is computed using cursor/offset so the alignment of
  // PrivateScriptData must be stricter than any trailing array type.
  Offset cursor = sizeof(PrivateScriptData);

  // Layout and initialize the gcthings array.
  {
    initElements<JS::GCCellPtr>(cursor, ngcthings);
    cursor += ngcthings * sizeof(JS::GCCellPtr);
  }

  // Sanity check.
  MOZ_ASSERT(endOffset() == cursor);
}

/* static */
PrivateScriptData* PrivateScriptData::new_(JSContext* cx, uint32_t ngcthings) {
  // Compute size including trailing arrays.
  CheckedInt<Offset> size = sizeof(PrivateScriptData);
  size += CheckedInt<Offset>(ngcthings) * sizeof(JS::GCCellPtr);
  if (!size.isValid()) {
    ReportAllocationOverflow(cx);
    return nullptr;
  }

  // Allocate contiguous raw buffer for the trailing arrays.
  void* raw = cx->pod_malloc<uint8_t>(size.value());
  MOZ_ASSERT(uintptr_t(raw) % alignof(PrivateScriptData) == 0);
  if (!raw) {
    return nullptr;
  }

  // Constuct the PrivateScriptData. Trailing arrays are uninitialized but
  // GCPtrs are put into a safe state.
  PrivateScriptData* result = new (raw) PrivateScriptData(ngcthings);
  if (!result) {
    return nullptr;
  }

  // Sanity check.
  MOZ_ASSERT(result->endOffset() == size.value());

  return result;
}

/* static */
bool PrivateScriptData::InitFromStencil(
    JSContext* cx, js::HandleScript script,
    const js::frontend::CompilationInput& input,
    const js::frontend::CompilationStencil& stencil,
    js::frontend::CompilationGCOutput& gcOutput,
    const js::frontend::ScriptIndex scriptIndex) {
  js::frontend::ScriptStencil& scriptStencil = stencil.scriptData[scriptIndex];
  uint32_t ngcthings = scriptStencil.gcThingsLength;

  MOZ_ASSERT(ngcthings <= INDEX_LIMIT);

  // Create and initialize PrivateScriptData
  if (!JSScript::createPrivateScriptData(cx, script, ngcthings)) {
    return false;
  }

  js::PrivateScriptData* data = script->data_;
  if (ngcthings) {
    if (!EmitScriptThingsVector(cx, input, stencil, gcOutput,
                                scriptStencil.gcthings(stencil),
                                data->gcthings())) {
      return false;
    }
  }

  return true;
}

void PrivateScriptData::trace(JSTracer* trc) {
  for (JS::GCCellPtr& elem : gcthings()) {
    gc::Cell* thing = elem.asCell();
    TraceManuallyBarrieredGenericPointerEdge(trc, &thing, "script-gcthing");
    if (MOZ_UNLIKELY(!thing)) {
      // NOTE: If we are clearing edges, also erase the type. This can happen
      // due to OOM triggering the ClearEdgesTracer.
      elem = JS::GCCellPtr();
    } else if (thing != elem.asCell()) {
      elem = JS::GCCellPtr(thing, elem.kind());
    }
  }
}

/*static*/
JSScript* JSScript::Create(JSContext* cx, js::HandleObject functionOrGlobal,
                           js::HandleScriptSourceObject sourceObject,
                           const SourceExtent& extent,
                           js::ImmutableScriptFlags flags) {
  return static_cast<JSScript*>(
      BaseScript::New(cx, functionOrGlobal, sourceObject, extent, flags));
}

#ifdef MOZ_VTUNE
uint32_t JSScript::vtuneMethodID() {
  if (!zone()->scriptVTuneIdMap) {
    auto map = MakeUnique<ScriptVTuneIdMap>();
    if (!map) {
      MOZ_CRASH("Failed to allocate ScriptVTuneIdMap");
    }

    zone()->scriptVTuneIdMap = std::move(map);
  }

  ScriptVTuneIdMap::AddPtr p = zone()->scriptVTuneIdMap->lookupForAdd(this);
  if (p) {
    return p->value();
  }

  MOZ_ASSERT(this->hasBytecode());

  uint32_t id = vtune::GenerateUniqueMethodID();
  if (!zone()->scriptVTuneIdMap->add(p, this, id)) {
    MOZ_CRASH("Failed to add vtune method id");
  }

  return id;
}
#endif

/* static */
bool JSScript::createPrivateScriptData(JSContext* cx, HandleScript script,
                                       uint32_t ngcthings) {
  cx->check(script);

  UniquePtr<PrivateScriptData> data(PrivateScriptData::new_(cx, ngcthings));
  if (!data) {
    return false;
  }

  script->swapData(data);
  MOZ_ASSERT(!data);

  return true;
}

/* static */
bool JSScript::fullyInitFromStencil(
    JSContext* cx, const js::frontend::CompilationInput& input,
    const js::frontend::CompilationStencil& stencil,
    frontend::CompilationGCOutput& gcOutput, HandleScript script,
    const js::frontend::ScriptIndex scriptIndex) {
  MutableScriptFlags lazyMutableFlags;
  RootedScope lazyEnclosingScope(cx);

  // A holder for the lazy PrivateScriptData that we must keep around in case
  // this process fails and we must return the script to its original state.
  //
  // This is initialized by BaseScript::swapData() which will run pre-barriers
  // for us. On successful conversion to non-lazy script, the old script data
  // here will be released by the UniquePtr.
  Rooted<UniquePtr<PrivateScriptData>> lazyData(cx);

#ifndef JS_CODEGEN_NONE
  // Whether we are a newborn script or an existing lazy script, we should
  // already be pointing to the interpreter trampoline.
  MOZ_ASSERT(script->isUsingInterpreterTrampoline(cx->runtime()));
#endif

  // If we are using an existing lazy script, record enough info to be able to
  // rollback on failure.
  if (script->isReadyForDelazification()) {
    lazyMutableFlags = script->mutableFlags_;
    lazyEnclosingScope = script->releaseEnclosingScope();
    script->swapData(lazyData.get());
    MOZ_ASSERT(script->sharedData_ == nullptr);
  }

  // Restore the script to lazy state on failure. If this was a fresh script, we
  // just need to clear bytecode to mark script as incomplete.
  auto rollbackGuard = mozilla::MakeScopeExit([&] {
    if (lazyEnclosingScope) {
      script->mutableFlags_ = lazyMutableFlags;
      script->warmUpData_.initEnclosingScope(lazyEnclosingScope);
      script->swapData(lazyData.get());
      script->sharedData_ = nullptr;

      MOZ_ASSERT(script->isReadyForDelazification());
    } else {
      script->sharedData_ = nullptr;
    }
  });

  // The counts of indexed things must be checked during code generation.
  MOZ_ASSERT(stencil.scriptData[scriptIndex].gcThingsLength <= INDEX_LIMIT);

  // Note: These flags should already be correct when the BaseScript was
  // allocated.
  MOZ_ASSERT_IF(stencil.isInitialStencil(),
                script->immutableFlags() ==
                    stencil.scriptExtra[scriptIndex].immutableFlags);

  // Create and initialize PrivateScriptData
  if (!PrivateScriptData::InitFromStencil(cx, script, input, stencil, gcOutput,
                                          scriptIndex)) {
    return false;
  }

  // Member-initializer data is computed in initial parse only. If we are
  // delazifying, make sure to copy it off the `lazyData` before we throw it
  // away.
  if (script->useMemberInitializers()) {
    if (stencil.isInitialStencil()) {
      MemberInitializers initializers(
          stencil.scriptExtra[scriptIndex].memberInitializers());
      script->setMemberInitializers(initializers);
    } else {
      script->setMemberInitializers(lazyData.get()->getMemberInitializers());
    }
  }

  script->initSharedData(stencil.sharedData.get(scriptIndex));

  // NOTE: JSScript is now constructed and should be linked in.
  rollbackGuard.release();

  // Link Scope -> JSFunction -> BaseScript.
  if (script->isFunction()) {
    JSFunction* fun = gcOutput.functions[scriptIndex];
    script->bodyScope()->as<FunctionScope>().initCanonicalFunction(fun);
    if (fun->isIncomplete()) {
      fun->initScript(script);
    } else {
      // We are delazifying in-place.
      MOZ_ASSERT(fun->baseScript() == script);
    }
  }

  // NOTE: The caller is responsible for linking ModuleObjects if this is a
  //       module script.

#ifdef JS_STRUCTURED_SPEW
  // We want this to happen after line number initialization to allow filtering
  // to work.
  script->setSpewEnabled(cx->spewer().enabled(script));
#endif

#ifdef DEBUG
  script->assertValidJumpTargets();
#endif

  if (coverage::IsLCovEnabled()) {
    if (!coverage::InitScriptCoverage(cx, script)) {
      return false;
    }
  }

  return true;
}

JSScript* JSScript::fromStencil(JSContext* cx,
                                frontend::CompilationInput& input,
                                const frontend::CompilationStencil& stencil,
                                frontend::CompilationGCOutput& gcOutput,
                                frontend::ScriptIndex scriptIndex) {
  js::frontend::ScriptStencil& scriptStencil = stencil.scriptData[scriptIndex];
  js::frontend::ScriptStencilExtra& scriptExtra =
      stencil.scriptExtra[scriptIndex];
  MOZ_ASSERT(scriptStencil.hasSharedData(),
             "Need generated bytecode to use JSScript::fromStencil");

  RootedObject functionOrGlobal(cx, cx->global());
  if (scriptStencil.isFunction()) {
    functionOrGlobal = gcOutput.functions[scriptIndex];
  }

  Rooted<ScriptSourceObject*> sourceObject(cx, gcOutput.sourceObject);
  RootedScript script(
      cx, Create(cx, functionOrGlobal, sourceObject, scriptExtra.extent,
                 scriptExtra.immutableFlags));
  if (!script) {
    return nullptr;
  }

  if (!fullyInitFromStencil(cx, input, stencil, gcOutput, script,
                            scriptIndex)) {
    return nullptr;
  }

  return script;
}

#ifdef DEBUG
void JSScript::assertValidJumpTargets() const {
  BytecodeLocation mainLoc = mainLocation();
  BytecodeLocation endLoc = endLocation();
  AllBytecodesIterable iter(this);
  for (BytecodeLocation loc : iter) {
    // Check jump instructions' target.
    if (loc.isJump()) {
      BytecodeLocation target = loc.getJumpTarget();
      MOZ_ASSERT(mainLoc <= target && target < endLoc);
      MOZ_ASSERT(target.isJumpTarget());

      // All backward jumps must be to a JSOp::LoopHead op. This is an invariant
      // we want to maintain to simplify JIT compilation and bytecode analysis.
      MOZ_ASSERT_IF(target < loc, target.is(JSOp::LoopHead));
      MOZ_ASSERT_IF(target < loc, IsBackedgePC(loc.toRawBytecode()));

      // All forward jumps must be to a JSOp::JumpTarget op.
      MOZ_ASSERT_IF(target > loc, target.is(JSOp::JumpTarget));

      // Jumps must not cross scope boundaries.
      MOZ_ASSERT(loc.innermostScope(this) == target.innermostScope(this));

      // Check fallthrough of conditional jump instructions.
      if (loc.fallsThrough()) {
        BytecodeLocation fallthrough = loc.next();
        MOZ_ASSERT(mainLoc <= fallthrough && fallthrough < endLoc);
        MOZ_ASSERT(fallthrough.isJumpTarget());
      }
    }

    // Check table switch case labels.
    if (loc.is(JSOp::TableSwitch)) {
      BytecodeLocation target = loc.getTableSwitchDefaultTarget();

      // Default target.
      MOZ_ASSERT(mainLoc <= target && target < endLoc);
      MOZ_ASSERT(target.is(JSOp::JumpTarget));

      int32_t low = loc.getTableSwitchLow();
      int32_t high = loc.getTableSwitchHigh();

      for (int i = 0; i < high - low + 1; i++) {
        BytecodeLocation switchCase = loc.getTableSwitchCaseTarget(this, i);
        MOZ_ASSERT(mainLoc <= switchCase && switchCase < endLoc);
        MOZ_ASSERT(switchCase.is(JSOp::JumpTarget));
      }
    }
  }

  // Check catch/finally blocks as jump targets.
  for (const TryNote& tn : trynotes()) {
    if (tn.kind() != TryNoteKind::Catch && tn.kind() != TryNoteKind::Finally) {
      continue;
    }

    jsbytecode* tryStart = offsetToPC(tn.start);
    jsbytecode* tryPc = tryStart - JSOpLength_Try;
    MOZ_ASSERT(JSOp(*tryPc) == JSOp::Try);

    jsbytecode* tryTarget = tryStart + tn.length;
    MOZ_ASSERT(main() <= tryTarget && tryTarget < codeEnd());
    MOZ_ASSERT(BytecodeIsJumpTarget(JSOp(*tryTarget)));
  }
}
#endif

void JSScript::addSizeOfJitScript(mozilla::MallocSizeOf mallocSizeOf,
                                  size_t* sizeOfJitScript,
                                  size_t* sizeOfBaselineFallbackStubs) const {
  if (!hasJitScript()) {
    return;
  }

  jitScript()->addSizeOfIncludingThis(mallocSizeOf, sizeOfJitScript,
                                      sizeOfBaselineFallbackStubs);
}

js::GlobalObject& JSScript::uninlinedGlobal() const { return global(); }

static const uint32_t GSN_CACHE_THRESHOLD = 100;

void GSNCache::purge() {
  code = nullptr;
  map.clearAndCompact();
}

const js::SrcNote* js::GetSrcNote(GSNCache& cache, JSScript* script,
                                  jsbytecode* pc) {
  size_t target = pc - script->code();
  if (target >= script->length()) {
    return nullptr;
  }

  if (cache.code == script->code()) {
    GSNCache::Map::Ptr p = cache.map.lookup(pc);
    return p ? p->value() : nullptr;
  }

  size_t offset = 0;
  const js::SrcNote* result;
  for (SrcNoteIterator iter(script->notes());; ++iter) {
    auto sn = *iter;
    if (sn->isTerminator()) {
      result = nullptr;
      break;
    }
    offset += sn->delta();
    if (offset == target && sn->isGettable()) {
      result = sn;
      break;
    }
  }

  if (cache.code != script->code() && script->length() >= GSN_CACHE_THRESHOLD) {
    unsigned nsrcnotes = 0;
    for (SrcNoteIterator iter(script->notes()); !iter.atEnd(); ++iter) {
      auto sn = *iter;
      if (sn->isGettable()) {
        ++nsrcnotes;
      }
    }
    if (cache.code) {
      cache.map.clear();
      cache.code = nullptr;
    }
    if (cache.map.reserve(nsrcnotes)) {
      pc = script->code();
      for (SrcNoteIterator iter(script->notes()); !iter.atEnd(); ++iter) {
        auto sn = *iter;
        pc += sn->delta();
        if (sn->isGettable()) {
          cache.map.putNewInfallible(pc, sn);
        }
      }
      cache.code = script->code();
    }
  }

  return result;
}

const js::SrcNote* js::GetSrcNote(JSContext* cx, JSScript* script,
                                  jsbytecode* pc) {
  return GetSrcNote(cx->caches().gsnCache, script, pc);
}

unsigned js::PCToLineNumber(unsigned startLine, unsigned startCol,
                            SrcNote* notes, jsbytecode* code, jsbytecode* pc,
                            unsigned* columnp) {
  unsigned lineno = startLine;
  unsigned column = startCol;

  /*
   * Walk through source notes accumulating their deltas, keeping track of
   * line-number notes, until we pass the note for pc's offset within
   * script->code.
   */
  ptrdiff_t offset = 0;
  ptrdiff_t target = pc - code;
  for (SrcNoteIterator iter(notes); !iter.atEnd(); ++iter) {
    auto sn = *iter;
    offset += sn->delta();
    if (offset > target) {
      break;
    }

    SrcNoteType type = sn->type();
    if (type == SrcNoteType::SetLine) {
      lineno = SrcNote::SetLine::getLine(sn, startLine);
      column = 0;
    } else if (type == SrcNoteType::NewLine) {
      lineno++;
      column = 0;
    } else if (type == SrcNoteType::ColSpan) {
      ptrdiff_t colspan = SrcNote::ColSpan::getSpan(sn);
      MOZ_ASSERT(ptrdiff_t(column) + colspan >= 0);
      column += colspan;
    }
  }

  if (columnp) {
    *columnp = column;
  }

  return lineno;
}

unsigned js::PCToLineNumber(JSScript* script, jsbytecode* pc,
                            unsigned* columnp) {
  /* Cope with InterpreterFrame.pc value prior to entering Interpret. */
  if (!pc) {
    return 0;
  }

  return PCToLineNumber(script->lineno(), script->column(), script->notes(),
                        script->code(), pc, columnp);
}

jsbytecode* js::LineNumberToPC(JSScript* script, unsigned target) {
  ptrdiff_t offset = 0;
  ptrdiff_t best = -1;
  unsigned lineno = script->lineno();
  unsigned bestdiff = SrcNote::MaxOperand;
  for (SrcNoteIterator iter(script->notes()); !iter.atEnd(); ++iter) {
    auto sn = *iter;
    /*
     * Exact-match only if offset is not in the prologue; otherwise use
     * nearest greater-or-equal line number match.
     */
    if (lineno == target && offset >= ptrdiff_t(script->mainOffset())) {
      goto out;
    }
    if (lineno >= target) {
      unsigned diff = lineno - target;
      if (diff < bestdiff) {
        bestdiff = diff;
        best = offset;
      }
    }
    offset += sn->delta();
    SrcNoteType type = sn->type();
    if (type == SrcNoteType::SetLine) {
      lineno = SrcNote::SetLine::getLine(sn, script->lineno());
    } else if (type == SrcNoteType::NewLine) {
      lineno++;
    }
  }
  if (best >= 0) {
    offset = best;
  }
out:
  return script->offsetToPC(offset);
}

JS_PUBLIC_API unsigned js::GetScriptLineExtent(JSScript* script) {
  unsigned lineno = script->lineno();
  unsigned maxLineNo = lineno;
  for (SrcNoteIterator iter(script->notes()); !iter.atEnd(); ++iter) {
    auto sn = *iter;
    SrcNoteType type = sn->type();
    if (type == SrcNoteType::SetLine) {
      lineno = SrcNote::SetLine::getLine(sn, script->lineno());
    } else if (type == SrcNoteType::NewLine) {
      lineno++;
    }

    if (maxLineNo < lineno) {
      maxLineNo = lineno;
    }
  }

  return 1 + maxLineNo - script->lineno();
}

#ifdef JS_CACHEIR_SPEW
void js::maybeUpdateWarmUpCount(JSScript* script) {
  if (script->needsFinalWarmUpCount()) {
    ScriptFinalWarmUpCountMap* map =
        script->zone()->scriptFinalWarmUpCountMap.get();
    // If needsFinalWarmUpCount is true, ScriptFinalWarmUpCountMap must have
    // already been created and thus must be asserted.
    MOZ_ASSERT(map);
    ScriptFinalWarmUpCountMap::Ptr p = map->lookup(script);
    MOZ_ASSERT(p);

    mozilla::Get<0>(p->value()) += script->jitScript()->warmUpCount();
  }
}

void js::maybeSpewScriptFinalWarmUpCount(JSScript* script) {
  if (script->needsFinalWarmUpCount()) {
    ScriptFinalWarmUpCountMap* map =
        script->zone()->scriptFinalWarmUpCountMap.get();
    // If needsFinalWarmUpCount is true, ScriptFinalWarmUpCountMap must have
    // already been created and thus must be asserted.
    MOZ_ASSERT(map);
    ScriptFinalWarmUpCountMap::Ptr p = map->lookup(script);
    MOZ_ASSERT(p);
    uint32_t warmUpCount;
    const char* scriptName;
    mozilla::Tie(warmUpCount, scriptName) = p->value();

    JSContext* cx = TlsContext.get();
    cx->spewer().enableSpewing();

    // In the case that we care about a script's final warmup count but the
    // spewer is not enabled, AutoSpewChannel automatically sets and unsets
    // the proper channel for the duration of spewing a health report's warm
    // up count.
    AutoSpewChannel channel(cx, SpewChannel::CacheIRHealthReport, script);
    jit::CacheIRHealth cih;
    cih.spewScriptFinalWarmUpCount(cx, scriptName, script, warmUpCount);

    script->zone()->scriptFinalWarmUpCountMap->remove(script);
    script->setNeedsFinalWarmUpCount(false);
  }
}
#endif

void js::DescribeScriptedCallerForDirectEval(JSContext* cx, HandleScript script,
                                             jsbytecode* pc, const char** file,
                                             unsigned* linenop,
                                             uint32_t* pcOffset,
                                             bool* mutedErrors) {
  MOZ_ASSERT(script->containsPC(pc));

  static_assert(JSOpLength_SpreadEval == JSOpLength_StrictSpreadEval,
                "next op after a spread must be at consistent offset");
  static_assert(JSOpLength_Eval == JSOpLength_StrictEval,
                "next op after a direct eval must be at consistent offset");

  MOZ_ASSERT(JSOp(*pc) == JSOp::Eval || JSOp(*pc) == JSOp::StrictEval ||
             JSOp(*pc) == JSOp::SpreadEval ||
             JSOp(*pc) == JSOp::StrictSpreadEval);

  bool isSpread =
      (JSOp(*pc) == JSOp::SpreadEval || JSOp(*pc) == JSOp::StrictSpreadEval);
  jsbytecode* nextpc =
      pc + (isSpread ? JSOpLength_SpreadEval : JSOpLength_Eval);
  MOZ_ASSERT(JSOp(*nextpc) == JSOp::Lineno);

  *file = script->filename();
  *linenop = GET_UINT32(nextpc);
  *pcOffset = script->pcToOffset(pc);
  *mutedErrors = script->mutedErrors();
}

void js::DescribeScriptedCallerForCompilation(
    JSContext* cx, MutableHandleScript maybeScript, const char** file,
    unsigned* linenop, uint32_t* pcOffset, bool* mutedErrors) {
  NonBuiltinFrameIter iter(cx, cx->realm()->principals());

  if (iter.done()) {
    maybeScript.set(nullptr);
    *file = nullptr;
    *linenop = 0;
    *pcOffset = 0;
    *mutedErrors = false;
    return;
  }

  *file = iter.filename();
  *linenop = iter.computeLine();
  *mutedErrors = iter.mutedErrors();

  // These values are only used for introducer fields which are debugging
  // information and can be safely left null for wasm frames.
  if (iter.hasScript()) {
    maybeScript.set(iter.script());
    *pcOffset = iter.pc() - maybeScript->code();
  } else {
    maybeScript.set(nullptr);
    *pcOffset = 0;
  }
}

static JSObject* CloneInnerInterpretedFunction(
    JSContext* cx, HandleScope enclosingScope, HandleFunction srcFun,
    Handle<ScriptSourceObject*> sourceObject) {
  /* NB: Keep this in sync with XDRInterpretedFunction. */
  RootedObject cloneProto(cx);
  if (!GetFunctionPrototype(cx, srcFun->generatorKind(), srcFun->asyncKind(),
                            &cloneProto)) {
    return nullptr;
  }

  RootedAtom atom(cx, srcFun->displayAtom());
  if (atom) {
    cx->markAtom(atom);
  }
  RootedFunction clone(
      cx, NewFunctionWithProto(cx, nullptr, srcFun->nargs(), srcFun->flags(),
                               nullptr, atom, cloneProto,
                               srcFun->getAllocKind(), TenuredObject));
  if (!clone) {
    return nullptr;
  }

  JSScript::AutoDelazify srcScript(cx, srcFun);
  if (!srcScript) {
    return nullptr;
  }
  JSScript* cloneScript = CloneScriptIntoFunction(cx, enclosingScope, clone,
                                                  srcScript, sourceObject);
  if (!cloneScript) {
    return nullptr;
  }

  MOZ_ASSERT(cloneScript->hasBytecode());

  return clone;
}

static JSObject* CloneScriptObject(JSContext* cx, PrivateScriptData* srcData,
                                   HandleObject obj,
                                   Handle<ScriptSourceObject*> sourceObject,
                                   JS::HandleVector<JS::GCCellPtr> gcThings) {
  if (obj->is<RegExpObject>()) {
    return CloneScriptRegExpObject(cx, obj->as<RegExpObject>());
  }

  if (obj->is<JSFunction>()) {
    HandleFunction innerFun = obj.as<JSFunction>();
    if (innerFun->isNativeFun()) {
      if (cx->realm() != innerFun->realm()) {
        MOZ_ASSERT(innerFun->isAsmJSNative());
        JS_ReportErrorASCII(cx, "AsmJS modules do not yet support cloning.");
        return nullptr;
      }
      return innerFun;
    }

    if (!innerFun->hasBytecode()) {
      MOZ_ASSERT(!innerFun->isSelfHostedOrIntrinsic(),
                 "Cannot enter realm of self-hosted functions");
      AutoRealm ar(cx, innerFun);
      if (!JSFunction::getOrCreateScript(cx, innerFun)) {
        return nullptr;
      }
    }

    Scope* enclosing = innerFun->enclosingScope();
    uint32_t scopeIndex = FindScopeIndex(srcData->gcthings(), *enclosing);
    RootedScope enclosingClone(cx, &gcThings[scopeIndex].get().as<Scope>());
    return CloneInnerInterpretedFunction(cx, enclosingClone, innerFun,
                                         sourceObject);
  }

  return DeepCloneObjectLiteral(cx, obj);
}

/* static */
bool PrivateScriptData::Clone(JSContext* cx, HandleScript src, HandleScript dst,
                              MutableHandle<GCVector<Scope*>> scopes) {
  PrivateScriptData* srcData = src->data_;
  uint32_t ngcthings = srcData->gcthings().size();

  // Clone GC things.
  JS::RootedVector<JS::GCCellPtr> gcThings(cx);
  size_t scopeIndex = 0;
  Rooted<ScriptSourceObject*> sourceObject(cx, dst->sourceObject());
  RootedObject obj(cx);
  RootedScope scope(cx);
  RootedScope enclosingScope(cx);
  RootedBigInt bigint(cx);
  for (JS::GCCellPtr gcThing : srcData->gcthings()) {
    if (gcThing.is<JSObject>()) {
      obj = &gcThing.as<JSObject>();
      JSObject* clone =
          CloneScriptObject(cx, srcData, obj, sourceObject, gcThings);
      if (!clone || !gcThings.append(JS::GCCellPtr(clone))) {
        return false;
      }
    } else if (gcThing.is<Scope>()) {
      // The passed in scopes vector contains body scopes that needed to be
      // cloned especially, depending on whether the script is a function or
      // global scope. Clone all other scopes.
      if (scopeIndex < scopes.length()) {
        if (!gcThings.append(JS::GCCellPtr(scopes[scopeIndex].get()))) {
          return false;
        }
      } else {
        scope = &gcThing.as<Scope>();
        uint32_t enclosingScopeIndex =
            FindScopeIndex(srcData->gcthings(), *scope->enclosing());
        enclosingScope = &gcThings[enclosingScopeIndex].get().as<Scope>();
        Scope* clone = Scope::clone(cx, scope, enclosingScope);
        if (!clone || !gcThings.append(JS::GCCellPtr(clone))) {
          return false;
        }
      }
      scopeIndex++;
    } else if (gcThing.is<JSString>()) {
      JSAtom* atom = &gcThing.as<JSString>().asAtom();
      if (cx->zone() != atom->zone()) {
        cx->markAtom(atom);
      }
      if (!gcThings.append(JS::GCCellPtr(atom))) {
        return false;
      }
    } else {
      bigint = &gcThing.as<BigInt>();
      BigInt* clone = bigint;
      if (cx->zone() != bigint->zone()) {
        clone = BigInt::copy(cx, bigint, gc::TenuredHeap);
        if (!clone) {
          return false;
        }
      }
      if (!gcThings.append(JS::GCCellPtr(clone))) {
        return false;
      }
    }
  }

  // Create the new PrivateScriptData on |dst| and fill it in.
  if (!JSScript::createPrivateScriptData(cx, dst, ngcthings)) {
    return false;
  }
  PrivateScriptData* dstData = dst->data_;

  dstData->memberInitializers_ = srcData->memberInitializers_;

  {
    auto array = dstData->gcthings();
    for (uint32_t i = 0; i < ngcthings; ++i) {
      array[i] = gcThings[i].get();
    }
  }

  return true;
}

static JSScript* CopyScriptImpl(JSContext* cx, HandleScript src,
                                HandleObject functionOrGlobal,
                                HandleScriptSourceObject sourceObject,
                                MutableHandle<GCVector<Scope*>> scopes,
                                SourceExtent* maybeClassExtent = nullptr) {
  if (src->treatAsRunOnce()) {
    MOZ_ASSERT(!src->isFunction());
    JS_ReportErrorASCII(cx, "No cloning toplevel run-once scripts");
    return nullptr;
  }

  /* NB: Keep this in sync with XDRScript. */

  // Some embeddings are not careful to use ExposeObjectToActiveJS as needed.
  JS::AssertObjectIsNotGray(sourceObject);

  SourceExtent extent = src->extent();

  ImmutableScriptFlags flags = src->immutableFlags();
  flags.setFlag(JSScript::ImmutableFlags::HasNonSyntacticScope,
                scopes[0]->hasOnChain(ScopeKind::NonSyntactic));

  // FunctionFlags and ImmutableScriptFlags should agree on self-hosting status.
  MOZ_ASSERT_IF(functionOrGlobal->is<JSFunction>(),
                functionOrGlobal->as<JSFunction>().isSelfHostedBuiltin() ==
                    flags.hasFlag(JSScript::ImmutableFlags::SelfHosted));

  // Create a new JSScript to fill in.
  RootedScript dst(
      cx, JSScript::Create(cx, functionOrGlobal, sourceObject, extent, flags));
  if (!dst) {
    return nullptr;
  }

  // Clone the PrivateScriptData into dst
  if (!PrivateScriptData::Clone(cx, src, dst, scopes)) {
    return nullptr;
  }

  // The SharedImmutableScriptData can be reused by any zone in the Runtime.
  dst->initSharedData(src->sharedData());

  return dst;
}

JSScript* js::CloneGlobalScript(JSContext* cx, HandleScript src) {
  MOZ_ASSERT(src->realm() != cx->realm(),
             "js::CloneGlobalScript should only be used for for realm "
             "mismatches. Otherwise just share the script directly.");

  Rooted<ScriptSourceObject*> sourceObject(cx, src->sourceObject());
  if (cx->compartment() != sourceObject->compartment()) {
    sourceObject = ScriptSourceObject::clone(cx, sourceObject);
    if (!sourceObject) {
      return nullptr;
    }
  }

  MOZ_ASSERT(src->bodyScopeIndex() == GCThingIndex::outermostScopeIndex());
  Rooted<GCVector<Scope*>> scopes(cx, GCVector<Scope*>(cx));
  Rooted<GlobalScope*> original(cx, &src->bodyScope()->as<GlobalScope>());
  GlobalScope* clone = GlobalScope::clone(cx, original);
  if (!clone || !scopes.append(clone)) {
    return nullptr;
  }

  RootedObject global(cx, cx->global());
  RootedScript dst(cx, CopyScriptImpl(cx, src, global, sourceObject, &scopes));
  if (!dst) {
    return nullptr;
  }

  if (coverage::IsLCovEnabled()) {
    if (!coverage::InitScriptCoverage(cx, dst)) {
      return nullptr;
    }
  }

  DebugAPI::onNewScript(cx, dst);

  return dst;
}

JSScript* js::CloneScriptIntoFunction(
    JSContext* cx, HandleScope enclosingScope, HandleFunction fun,
    HandleScript src, Handle<ScriptSourceObject*> sourceObject) {
  MOZ_ASSERT(src->realm() != cx->realm(),
             "js::CloneScriptIntoFunction should only be used for for realm "
             "mismatches. Otherwise just share the script directly.");

  // We are either delazifying a self-hosted lazy function or the function
  // should be in an inactive state.
  MOZ_ASSERT(fun->isIncomplete() || fun->hasSelfHostedLazyScript());

  // Clone the non-intra-body scopes.
  Rooted<GCVector<Scope*>> scopes(cx, GCVector<Scope*>(cx));
  RootedScope original(cx);
  RootedScope enclosingClone(cx);
  for (uint32_t i = 0; i <= src->bodyScopeIndex().index; i++) {
    original = src->getScope(GCThingIndex(i));

    if (i == 0) {
      enclosingClone = enclosingScope;
    } else {
      MOZ_ASSERT(src->getScope(GCThingIndex(i - 1)) == original->enclosing());
      enclosingClone = scopes[i - 1];
    }

    Scope* clone;
    if (original->is<FunctionScope>()) {
      clone = FunctionScope::clone(cx, original.as<FunctionScope>(), fun,
                                   enclosingClone);
    } else {
      clone = Scope::clone(cx, original, enclosingClone);
    }

    if (!clone || !scopes.append(clone)) {
      return nullptr;
    }
  }

  // Save flags in case we need to undo the early mutations.
  const FunctionFlags preservedFlags = fun->flags();
  RootedScript dst(cx, CopyScriptImpl(cx, src, fun, sourceObject, &scopes));
  if (!dst) {
    fun->setFlags(preservedFlags);
    return nullptr;
  }

  // Finally set the script after all the fallible operations.
  if (fun->isIncomplete()) {
    fun->initScript(dst);
  } else {
    MOZ_ASSERT(fun->hasSelfHostedLazyScript());
    fun->clearSelfHostedLazyScript();
    fun->initScript(dst);
  }

  if (coverage::IsLCovEnabled()) {
    if (!coverage::InitScriptCoverage(cx, dst)) {
      return nullptr;
    }
  }

  return dst;
}

template <typename SourceSpan, typename TargetSpan>
void CopySpan(const SourceSpan& source, TargetSpan target) {
  MOZ_ASSERT(source.size() == target.size());
  std::copy(source.cbegin(), source.cend(), target.begin());
}

/* static */
js::UniquePtr<ImmutableScriptData> ImmutableScriptData::new_(
    JSContext* cx, uint32_t mainOffset, uint32_t nfixed, uint32_t nslots,
    GCThingIndex bodyScopeIndex, uint32_t numICEntries, bool isFunction,
    uint16_t funLength, mozilla::Span<const jsbytecode> code,
    mozilla::Span<const SrcNote> notes,
    mozilla::Span<const uint32_t> resumeOffsets,
    mozilla::Span<const ScopeNote> scopeNotes,
    mozilla::Span<const TryNote> tryNotes) {
  MOZ_RELEASE_ASSERT(code.Length() <= frontend::MaxBytecodeLength);

  // There are 1-4 copies of SrcNoteType::Null appended after the source
  // notes. These are a combination of sentinel and padding values.
  static_assert(frontend::MaxSrcNotesLength <= UINT32_MAX - CodeNoteAlign,
                "Length + CodeNoteAlign shouldn't overflow UINT32_MAX");
  size_t noteLength = notes.Length();
  MOZ_RELEASE_ASSERT(noteLength <= frontend::MaxSrcNotesLength);

  size_t nullLength = ComputeNotePadding(code.Length(), noteLength);

  // Allocate ImmutableScriptData
  js::UniquePtr<ImmutableScriptData> data(ImmutableScriptData::new_(
      cx, code.Length(), noteLength + nullLength, resumeOffsets.Length(),
      scopeNotes.Length(), tryNotes.Length()));
  if (!data) {
    return data;
  }

  // Initialize POD fields
  data->mainOffset = mainOffset;
  data->nfixed = nfixed;
  data->nslots = nslots;
  data->bodyScopeIndex = bodyScopeIndex;
  data->numICEntries = numICEntries;

  if (isFunction) {
    data->funLength = funLength;
  }

  // Initialize trailing arrays
  CopySpan(code, data->codeSpan());
  CopySpan(notes, data->notesSpan().To(noteLength));
  std::fill_n(data->notes() + noteLength, nullLength, SrcNote::terminator());
  CopySpan(resumeOffsets, data->resumeOffsets());
  CopySpan(scopeNotes, data->scopeNotes());
  CopySpan(tryNotes, data->tryNotes());

  return data;
}

void ScriptWarmUpData::trace(JSTracer* trc) {
  uintptr_t tag = data_ & TagMask;
  switch (tag) {
    case EnclosingScriptTag: {
      BaseScript* enclosingScript = toEnclosingScript();
      TraceManuallyBarrieredEdge(trc, &enclosingScript, "enclosingScript");
      setTaggedPtr<EnclosingScriptTag>(enclosingScript);
      break;
    }

    case EnclosingScopeTag: {
      Scope* enclosingScope = toEnclosingScope();
      TraceManuallyBarrieredEdge(trc, &enclosingScope, "enclosingScope");
      setTaggedPtr<EnclosingScopeTag>(enclosingScope);
      break;
    }

    case JitScriptTag: {
      toJitScript()->trace(trc);
      break;
    }

    default: {
      MOZ_ASSERT(isWarmUpCount());
      break;
    }
  }
}

size_t JSScript::calculateLiveFixed(jsbytecode* pc) {
  size_t nlivefixed = numAlwaysLiveFixedSlots();

  if (nfixed() != nlivefixed) {
    Scope* scope = lookupScope(pc);
    if (scope) {
      scope = MaybeForwarded(scope);
    }

    // Find the nearest LexicalScope in the same script.
    while (scope && scope->is<WithScope>()) {
      scope = scope->enclosing();
      if (scope) {
        scope = MaybeForwarded(scope);
      }
    }

    if (scope) {
      if (scope->is<LexicalScope>()) {
        nlivefixed = scope->as<LexicalScope>().nextFrameSlot();
      } else if (scope->is<VarScope>()) {
        nlivefixed = scope->as<VarScope>().nextFrameSlot();
      } else if (scope->is<ClassBodyScope>()) {
        nlivefixed = scope->as<ClassBodyScope>().nextFrameSlot();
      }
    }
  }

  MOZ_ASSERT(nlivefixed <= nfixed());
  MOZ_ASSERT(nlivefixed >= numAlwaysLiveFixedSlots());

  return nlivefixed;
}

Scope* JSScript::lookupScope(jsbytecode* pc) const {
  MOZ_ASSERT(containsPC(pc));

  size_t offset = pc - code();

  auto notes = scopeNotes();
  Scope* scope = nullptr;

  // Find the innermost block chain using a binary search.
  size_t bottom = 0;
  size_t top = notes.size();

  while (bottom < top) {
    size_t mid = bottom + (top - bottom) / 2;
    const ScopeNote* note = &notes[mid];
    if (note->start <= offset) {
      // Block scopes are ordered in the list by their starting offset, and
      // since blocks form a tree ones earlier in the list may cover the pc even
      // if later blocks end before the pc. This only happens when the earlier
      // block is a parent of the later block, so we need to check parents of
      // |mid| in the searched range for coverage.
      size_t check = mid;
      while (check >= bottom) {
        const ScopeNote* checkNote = &notes[check];
        MOZ_ASSERT(checkNote->start <= offset);
        if (offset < checkNote->start + checkNote->length) {
          // We found a matching block chain but there may be inner ones
          // at a higher block chain index than mid. Continue the binary search.
          if (checkNote->index == ScopeNote::NoScopeIndex) {
            scope = nullptr;
          } else {
            scope = getScope(checkNote->index);
          }
          break;
        }
        if (checkNote->parent == UINT32_MAX) {
          break;
        }
        check = checkNote->parent;
      }
      bottom = mid + 1;
    } else {
      top = mid;
    }
  }

  return scope;
}

Scope* JSScript::innermostScope(jsbytecode* pc) const {
  if (Scope* scope = lookupScope(pc)) {
    return scope;
  }
  return bodyScope();
}

void js::SetFrameArgumentsObject(JSContext* cx, AbstractFramePtr frame,
                                 HandleScript script, JSObject* argsobj) {
  /*
   * If the arguments object was optimized out by scalar replacement,
   * we must recreate it when we bail out. Because 'arguments' may have
   * already been overwritten, we must check to see if the slot already
   * contains a value.
   */

  Rooted<BindingIter> bi(cx, BindingIter(script));
  while (bi && bi.name() != cx->names().arguments) {
    bi++;
  }
  if (!bi) {
    return;
  }

  if (bi.location().kind() == BindingLocation::Kind::Environment) {
#ifdef DEBUG
    /*
     * If |arguments| lives in the call object, we should not have
     * optimized it. Scan the script to find the slot in the call
     * object that |arguments| is assigned to and verify that it
     * already exists.
     */
    jsbytecode* pc = script->code();
    while (JSOp(*pc) != JSOp::Arguments) {
      pc += GetBytecodeLength(pc);
    }
    pc += JSOpLength_Arguments;
    MOZ_ASSERT(JSOp(*pc) == JSOp::SetAliasedVar);

    EnvironmentObject& env = frame.callObj().as<EnvironmentObject>();
    MOZ_ASSERT(!env.aliasedBinding(bi).isMagic(JS_OPTIMIZED_OUT));
#endif
    return;
  }

  MOZ_ASSERT(bi.location().kind() == BindingLocation::Kind::Frame);
  uint32_t frameSlot = bi.location().slot();
  if (frame.unaliasedLocal(frameSlot).isMagic(JS_OPTIMIZED_OUT)) {
    frame.unaliasedLocal(frameSlot) = ObjectValue(*argsobj);
  }
}

bool JSScript::formalIsAliased(unsigned argSlot) {
  if (functionHasParameterExprs()) {
    return false;
  }

  for (PositionalFormalParameterIter fi(this); fi; fi++) {
    if (fi.argumentSlot() == argSlot) {
      return fi.closedOver();
    }
  }
  MOZ_CRASH("Argument slot not found");
}

// Returns true if any formal argument is mapped by the arguments
// object, but lives in the call object.
bool JSScript::anyFormalIsForwarded() {
  if (!argsObjAliasesFormals()) {
    return false;
  }

  for (PositionalFormalParameterIter fi(this); fi; fi++) {
    if (fi.closedOver()) {
      return true;
    }
  }
  return false;
}

bool JSScript::formalLivesInArgumentsObject(unsigned argSlot) {
  return argsObjAliasesFormals() && !formalIsAliased(argSlot);
}

/* static */
BaseScript* BaseScript::New(JSContext* cx, HandleObject functionOrGlobal,
                            HandleScriptSourceObject sourceObject,
                            const SourceExtent& extent,
                            uint32_t immutableFlags) {
  void* script = Allocate<BaseScript>(cx);
  if (!script) {
    return nullptr;
  }

#ifndef JS_CODEGEN_NONE
  uint8_t* stubEntry = cx->runtime()->jitRuntime()->interpreterStub().value;
#else
  uint8_t* stubEntry = nullptr;
#endif

  return new (script) BaseScript(stubEntry, functionOrGlobal, sourceObject,
                                 extent, immutableFlags);
}

/* static */
BaseScript* BaseScript::CreateRawLazy(JSContext* cx, uint32_t ngcthings,
                                      HandleFunction fun,
                                      HandleScriptSourceObject sourceObject,
                                      const SourceExtent& extent,
                                      uint32_t immutableFlags) {
  cx->check(fun);

  BaseScript* lazy = New(cx, fun, sourceObject, extent, immutableFlags);
  if (!lazy) {
    return nullptr;
  }

  // Allocate a PrivateScriptData if it will not be empty. Lazy class
  // constructors that use member initializers also need PrivateScriptData for
  // field data.
  if (ngcthings || lazy->useMemberInitializers()) {
    UniquePtr<PrivateScriptData> data(PrivateScriptData::new_(cx, ngcthings));
    if (!data) {
      return nullptr;
    }
    lazy->swapData(data);
    MOZ_ASSERT(!data);
  }

  return lazy;
}

void JSScript::updateJitCodeRaw(JSRuntime* rt) {
  MOZ_ASSERT(rt);
  if (hasBaselineScript() && baselineScript()->hasPendingIonCompileTask()) {
    MOZ_ASSERT(!isIonCompilingOffThread());
    setJitCodeRaw(rt->jitRuntime()->lazyLinkStub().value);
  } else if (hasIonScript()) {
    jit::IonScript* ion = ionScript();
    setJitCodeRaw(ion->method()->raw());
  } else if (hasBaselineScript()) {
    setJitCodeRaw(baselineScript()->method()->raw());
  } else if (hasJitScript() && js::jit::IsBaselineInterpreterEnabled()) {
    setJitCodeRaw(rt->jitRuntime()->baselineInterpreter().codeRaw());
  } else {
    setJitCodeRaw(rt->jitRuntime()->interpreterStub().value);
  }
  MOZ_ASSERT(jitCodeRaw());
}

bool JSScript::hasLoops() {
  for (const TryNote& tn : trynotes()) {
    if (tn.isLoop()) {
      return true;
    }
  }
  return false;
}

bool JSScript::mayReadFrameArgsDirectly() {
  return needsArgsObj() || hasRest();
}

void JSScript::resetWarmUpCounterToDelayIonCompilation() {
  // Reset the warm-up count only if it's greater than the BaselineCompiler
  // threshold. We do this to ensure this has no effect on Baseline compilation
  // because we don't want scripts to get stuck in the (Baseline) interpreter in
  // pathological cases.

  if (getWarmUpCount() > jit::JitOptions.baselineJitWarmUpThreshold) {
    incWarmUpResetCounter();
    uint32_t newCount = jit::JitOptions.baselineJitWarmUpThreshold;
    if (warmUpData_.isWarmUpCount()) {
      warmUpData_.resetWarmUpCount(newCount);
    } else {
      warmUpData_.toJitScript()->resetWarmUpCount(newCount);
    }
  }
}

gc::AllocSite* JSScript::createAllocSite() {
  return jitScript()->createAllocSite(this);
}

void JSScript::AutoDelazify::holdScript(JS::HandleFunction fun) {
  if (fun) {
    if (fun->realm()->isSelfHostingRealm()) {
      // The self-hosting realm is shared across runtimes, so we can't use
      // JSAutoRealm: it could cause races. Functions in the self-hosting
      // realm will never be lazy, so we can safely assume we don't have
      // to delazify.
      script_ = fun->nonLazyScript();
    } else {
      JSAutoRealm ar(cx_, fun);
      script_ = JSFunction::getOrCreateScript(cx_, fun);
      if (script_) {
        oldAllowRelazify_ = script_->allowRelazify();
        script_->clearAllowRelazify();
      }
    }
  }
}

void JSScript::AutoDelazify::dropScript() {
  // Don't touch script_ if it's in the self-hosting realm, see the comment
  // in holdScript.
  if (script_ && !script_->realm()->isSelfHostingRealm()) {
    script_->setAllowRelazify(oldAllowRelazify_);
  }
  script_ = nullptr;
}

JS::ubi::Base::Size JS::ubi::Concrete<BaseScript>::size(
    mozilla::MallocSizeOf mallocSizeOf) const {
  BaseScript* base = &get();

  Size size = gc::Arena::thingSize(base->getAllocKind());
  size += base->sizeOfExcludingThis(mallocSizeOf);

  // Include any JIT data if it exists.
  if (base->hasJitScript()) {
    JSScript* script = base->asJSScript();

    size_t jitScriptSize = 0;
    size_t fallbackStubSize = 0;
    script->addSizeOfJitScript(mallocSizeOf, &jitScriptSize, &fallbackStubSize);
    size += jitScriptSize;
    size += fallbackStubSize;

    size_t baselineSize = 0;
    jit::AddSizeOfBaselineData(script, mallocSizeOf, &baselineSize);
    size += baselineSize;

    size += jit::SizeOfIonData(script, mallocSizeOf);
  }

  MOZ_ASSERT(size > 0);
  return size;
}

const char* JS::ubi::Concrete<BaseScript>::scriptFilename() const {
  return get().filename();
}
