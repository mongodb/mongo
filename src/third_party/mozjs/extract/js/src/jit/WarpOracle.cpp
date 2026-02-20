/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpOracle.h"

#include "mozilla/ScopeExit.h"
#include "mozilla/Try.h"

#include <algorithm>

#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIRReader.h"
#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitHints.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "jit/JitSpewer.h"
#include "jit/JitZone.h"
#include "jit/MIRGenerator.h"
#include "jit/TrialInlining.h"
#include "jit/TypeData.h"
#include "jit/WarpBuilder.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin
#include "util/DifferentialTesting.h"
#include "vm/BuiltinObjectKind.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"

#include "jit/InlineScriptTree-inl.h"
#include "vm/BytecodeIterator-inl.h"
#include "vm/BytecodeLocation-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::Maybe;

// WarpScriptOracle creates a WarpScriptSnapshot for a single JSScript. Note
// that a single WarpOracle can use multiple WarpScriptOracles when scripts are
// inlined.
class MOZ_STACK_CLASS WarpScriptOracle {
  JSContext* cx_;
  WarpOracle* oracle_;
  MIRGenerator& mirGen_;
  TempAllocator& alloc_;
  HandleScript script_;
  const CompileInfo* info_;
  ICScript* icScript_;

  // Index of the next ICEntry for getICEntry. This assumes the script's
  // bytecode is processed from first to last instruction.
  uint32_t icEntryIndex_ = 0;

  template <typename... Args>
  mozilla::GenericErrorResult<AbortReason> abort(Args&&... args) {
    return oracle_->abort(script_, args...);
  }

  WarpEnvironment createEnvironment();
  AbortReasonOr<Ok> maybeInlineIC(WarpOpSnapshotList& snapshots,
                                  BytecodeLocation loc);
  AbortReasonOr<bool> maybeInlineCall(WarpOpSnapshotList& snapshots,
                                      BytecodeLocation loc, ICCacheIRStub* stub,
                                      ICFallbackStub* fallbackStub,
                                      uint8_t* stubDataCopy);
  AbortReasonOr<bool> maybeInlinePolymorphicTypes(WarpOpSnapshotList& snapshots,
                                                  BytecodeLocation loc,
                                                  ICCacheIRStub* firstStub,
                                                  ICFallbackStub* fallbackStub);
  [[nodiscard]] bool replaceNurseryAndAllocSitePointers(
      ICCacheIRStub* stub, const CacheIRStubInfo* stubInfo,
      uint8_t* stubDataCopy);
  bool maybeReplaceNurseryPointer(const CacheIRStubInfo* stubInfo,
                                  uint8_t* stubDataCopy, JSObject* obj,
                                  size_t offset);

 public:
  WarpScriptOracle(JSContext* cx, WarpOracle* oracle, HandleScript script,
                   const CompileInfo* info, ICScript* icScript)
      : cx_(cx),
        oracle_(oracle),
        mirGen_(oracle->mirGen()),
        alloc_(mirGen_.alloc()),
        script_(script),
        info_(info),
        icScript_(icScript) {}

  AbortReasonOr<WarpScriptSnapshot*> createScriptSnapshot();

  ICEntry& getICEntryAndFallback(BytecodeLocation loc,
                                 ICFallbackStub** fallback);
};

WarpOracle::WarpOracle(JSContext* cx, MIRGenerator& mirGen,
                       HandleScript outerScript)
    : cx_(cx),
      mirGen_(mirGen),
      alloc_(mirGen.alloc()),
      outerScript_(outerScript) {}

mozilla::GenericErrorResult<AbortReason> WarpOracle::abort(HandleScript script,
                                                           AbortReason r) {
  auto res = mirGen_.abort(r);
  JitSpew(JitSpew_IonAbort, "aborted @ %s", script->filename());
  return res;
}

mozilla::GenericErrorResult<AbortReason> WarpOracle::abort(HandleScript script,
                                                           AbortReason r,
                                                           const char* message,
                                                           ...) {
  va_list ap;
  va_start(ap, message);
  auto res = mirGen_.abortFmt(r, message, ap);
  va_end(ap);
  JitSpew(JitSpew_IonAbort, "aborted @ %s", script->filename());
  return res;
}

void WarpOracle::addScriptSnapshot(WarpScriptSnapshot* scriptSnapshot,
                                   ICScript* icScript, size_t bytecodeLength) {
  scriptSnapshots_.insertBack(scriptSnapshot);
  accumulatedBytecodeSize_ += bytecodeLength;
#ifdef DEBUG
  runningScriptHash_ = mozilla::AddToHash(runningScriptHash_, icScript->hash());
#endif
}

AbortReasonOr<WarpSnapshot*> WarpOracle::createSnapshot() {
#ifdef JS_JITSPEW
  const char* mode;
  if (outerScript_->hasIonScript()) {
    mode = "Recompiling";
  } else {
    mode = "Compiling";
  }
  JitSpew(JitSpew_IonScripts,
          "Warp %s script %s:%u:%u (%p) (warmup-counter=%" PRIu32 ",%s%s)",
          mode, outerScript_->filename(), outerScript_->lineno(),
          outerScript_->column().oneOriginValue(),
          static_cast<JSScript*>(outerScript_), outerScript_->getWarmUpCount(),
          outerScript_->isGenerator() ? " isGenerator" : "",
          outerScript_->isAsync() ? " isAsync" : "");
#endif

  accumulatedBytecodeSize_ = outerScript_->length();

  MOZ_ASSERT(outerScript_->hasJitScript());
  ICScript* icScript = outerScript_->jitScript()->icScript();
  WarpScriptOracle scriptOracle(cx_, this, outerScript_, &mirGen_.outerInfo(),
                                icScript);

  WarpScriptSnapshot* scriptSnapshot;
  MOZ_TRY_VAR(scriptSnapshot, scriptOracle.createScriptSnapshot());

  // Insert the outermost scriptSnapshot at the front of the list.
  scriptSnapshots_.insertFront(scriptSnapshot);

  bool recordFinalWarmUpCount = false;
#ifdef JS_CACHEIR_SPEW
  recordFinalWarmUpCount = outerScript_->needsFinalWarmUpCount();
#endif

  auto* snapshot = new (alloc_.fallible())
      WarpSnapshot(cx_, alloc_, std::move(scriptSnapshots_), zoneStubs_,
                   bailoutInfo_, recordFinalWarmUpCount);
  if (!snapshot) {
    return abort(outerScript_, AbortReason::Alloc);
  }

  if (!snapshot->nurseryObjects().appendAll(nurseryObjects_)) {
    return abort(outerScript_, AbortReason::Alloc);
  }

#ifdef JS_JITSPEW
  if (JitSpewEnabled(JitSpew_WarpSnapshots)) {
    Fprinter& out = JitSpewPrinter();
    snapshot->dump(out);
  }
#endif

#ifdef DEBUG
  // When transpiled CacheIR bails out, we do not want to recompile
  // with the exact same data and get caught in an invalidation loop.
  //
  // To avoid this, we store a hash of the stub pointers and entry
  // counts in this snapshot, save that hash in the JitScript if we
  // have a TranspiledCacheIR or MonomorphicInlinedStubFolding bailout,
  // and assert that the hash has changed when we recompile.
  //
  // Note: this assertion catches potential performance issues.
  // Failing this assertion is not a correctness/security problem.
  // We therefore ignore cases involving resource exhaustion (OOM,
  // stack overflow, etc), or stubs purged by GC.
  HashNumber hash = mozilla::AddToHash(icScript->hash(), runningScriptHash_);
  if (outerScript_->jitScript()->hasFailedICHash()) {
    HashNumber oldHash = outerScript_->jitScript()->getFailedICHash();
    MOZ_ASSERT_IF(hash == oldHash && !js::SupportDifferentialTesting(),
                  cx_->hadResourceExhaustion());
  }
  snapshot->setICHash(hash);
#endif

  return snapshot;
}

template <typename T, typename... Args>
[[nodiscard]] static bool AddOpSnapshot(TempAllocator& alloc,
                                        WarpOpSnapshotList& snapshots,
                                        uint32_t offset, Args&&... args) {
  T* snapshot = new (alloc.fallible()) T(offset, std::forward<Args>(args)...);
  if (!snapshot) {
    return false;
  }

  snapshots.insertBack(snapshot);
  return true;
}

[[nodiscard]] static bool AddWarpGetImport(TempAllocator& alloc,
                                           WarpOpSnapshotList& snapshots,
                                           uint32_t offset, JSScript* script,
                                           PropertyName* name) {
  ModuleEnvironmentObject* env = GetModuleEnvironmentForScript(script);
  MOZ_ASSERT(env);

  mozilla::Maybe<PropertyInfo> prop;
  ModuleEnvironmentObject* targetEnv;
  MOZ_ALWAYS_TRUE(env->lookupImport(NameToId(name), &targetEnv, &prop));

  uint32_t numFixedSlots = targetEnv->numFixedSlots();
  uint32_t slot = prop->slot();

  // In the rare case where this import hasn't been initialized already (we have
  // an import cycle where modules reference each other's imports), we need a
  // check.
  bool needsLexicalCheck =
      targetEnv->getSlot(slot).isMagic(JS_UNINITIALIZED_LEXICAL);

  return AddOpSnapshot<WarpGetImport>(alloc, snapshots, offset, targetEnv,
                                      numFixedSlots, slot, needsLexicalCheck);
}

ICEntry& WarpScriptOracle::getICEntryAndFallback(BytecodeLocation loc,
                                                 ICFallbackStub** fallback) {
  const uint32_t offset = loc.bytecodeToOffset(script_);

  do {
    *fallback = icScript_->fallbackStub(icEntryIndex_);
    icEntryIndex_++;
  } while ((*fallback)->pcOffset() < offset);

  MOZ_ASSERT((*fallback)->pcOffset() == offset);
  return icScript_->icEntry(icEntryIndex_ - 1);
}

WarpEnvironment WarpScriptOracle::createEnvironment() {
  // Don't do anything if the script doesn't use the environment chain.
  if (!script_->jitScript()->usesEnvironmentChain()) {
    return WarpEnvironment(NoEnvironment());
  }

  if (script_->isModule()) {
    ModuleObject* module = script_->module();
    JSObject* obj = &module->initialEnvironment();
    return WarpEnvironment(ConstantObjectEnvironment(obj));
  }

  JSFunction* fun = script_->function();
  if (!fun) {
    // For global scripts without a non-syntactic global scope, the environment
    // chain is the global lexical environment.
    MOZ_ASSERT(!script_->isForEval());
    MOZ_ASSERT(!script_->hasNonSyntacticScope());
    JSObject* obj = &script_->global().lexicalEnvironment();
    return WarpEnvironment(ConstantObjectEnvironment(obj));
  }

  auto [callObjectTemplate, namedLambdaTemplate] =
      script_->jitScript()->functionEnvironmentTemplates(fun);

  gc::Heap initialHeap = gc::Heap::Default;
  JitScript* jitScript = script_->jitScript();
  if (jitScript->hasEnvAllocSite()) {
    initialHeap = jitScript->icScript()->maybeEnvAllocSite()->initialHeap();
  }

  return WarpEnvironment(FunctionEnvironment(callObjectTemplate,
                                             namedLambdaTemplate, initialHeap));
}

AbortReasonOr<WarpScriptSnapshot*> WarpScriptOracle::createScriptSnapshot() {
  MOZ_ASSERT(script_->hasJitScript());

  if (!script_->jitScript()->ensureHasCachedIonData(cx_, script_)) {
    return abort(AbortReason::Error);
  }

  if (script_->failedBoundsCheck()) {
    oracle_->bailoutInfo().setFailedBoundsCheck();
  }
  if (script_->failedLexicalCheck()) {
    oracle_->bailoutInfo().setFailedLexicalCheck();
  }

  WarpEnvironment environment = createEnvironment();

  // Unfortunately LinkedList<> asserts the list is empty in its destructor.
  // Clear the list if we abort compilation.
  WarpOpSnapshotList opSnapshots;
  auto autoClearOpSnapshots =
      mozilla::MakeScopeExit([&] { opSnapshots.clear(); });

  ModuleObject* moduleObject = nullptr;

  // Analyze the bytecode. Abort compilation for unsupported ops and create
  // WarpOpSnapshots.
  for (BytecodeLocation loc : AllBytecodesIterable(script_)) {
    JSOp op = loc.getOp();
    uint32_t offset = loc.bytecodeToOffset(script_);
    switch (op) {
      case JSOp::Arguments: {
        MOZ_ASSERT(script_->needsArgsObj());
        bool mapped = script_->hasMappedArgsObj();
        ArgumentsObject* templateObj =
            script_->global().maybeArgumentsTemplateObject(mapped);
        if (!AddOpSnapshot<WarpArguments>(alloc_, opSnapshots, offset,
                                          templateObj)) {
          return abort(AbortReason::Alloc);
        }
        break;
      }
      case JSOp::RegExp: {
        bool hasShared = loc.getRegExp(script_)->hasShared();
        if (!AddOpSnapshot<WarpRegExp>(alloc_, opSnapshots, offset,
                                       hasShared)) {
          return abort(AbortReason::Alloc);
        }
        break;
      }

      case JSOp::FunctionThis:
        if (!script_->strict() && script_->hasNonSyntacticScope()) {
          // Abort because MBoxNonStrictThis doesn't support non-syntactic
          // scopes (a deprecated SpiderMonkey mechanism). If this becomes an
          // issue we could support it by refactoring GetFunctionThis to not
          // take a frame pointer and then call that.
          return abort(AbortReason::Disable,
                       "JSOp::FunctionThis with non-syntactic scope");
        }
        break;

      case JSOp::GlobalThis:
        MOZ_ASSERT(!script_->hasNonSyntacticScope());
        break;

      case JSOp::BuiltinObject: {
        // If we already resolved this built-in we can bake it in.
        auto kind = loc.getBuiltinObjectKind();
        if (JSObject* proto = MaybeGetBuiltinObject(cx_->global(), kind)) {
          if (!AddOpSnapshot<WarpBuiltinObject>(alloc_, opSnapshots, offset,
                                                proto)) {
            return abort(AbortReason::Alloc);
          }
        }
        break;
      }

      case JSOp::GetIntrinsic: {
        // If we already cloned this intrinsic we can bake it in.
        // NOTE: When the initializer runs in a content global, we also have to
        //       worry about nursery objects. These quickly tenure and stay that
        //       way so this is only a temporary problem.
        PropertyName* name = loc.getPropertyName(script_);
        Value val;
        if (cx_->global()->maybeGetIntrinsicValue(name, &val, cx_) &&
            JS::GCPolicy<Value>::isTenured(val)) {
          if (!AddOpSnapshot<WarpGetIntrinsic>(alloc_, opSnapshots, offset,
                                               val)) {
            return abort(AbortReason::Alloc);
          }
        }
        break;
      }

      case JSOp::ImportMeta: {
        if (!moduleObject) {
          moduleObject = GetModuleObjectForScript(script_);
          MOZ_ASSERT(moduleObject->isTenured());
        }
        break;
      }

      case JSOp::GetImport: {
        PropertyName* name = loc.getPropertyName(script_);
        if (!AddWarpGetImport(alloc_, opSnapshots, offset, script_, name)) {
          return abort(AbortReason::Alloc);
        }
        break;
      }

      case JSOp::Lambda: {
        JSFunction* fun = loc.getFunction(script_);
        if (IsAsmJSModule(fun)) {
          return abort(AbortReason::Disable, "asm.js module function lambda");
        }
        MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        break;
      }

      case JSOp::GetElemSuper: {
#if defined(JS_CODEGEN_X86)
        // x86 does not have enough registers.
        return abort(AbortReason::Disable,
                     "GetElemSuper is not supported on x86");
#else
        MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        break;
#endif
      }

      case JSOp::Rest: {
        if (Shape* shape =
                script_->global().maybeArrayShapeWithDefaultProto()) {
          if (!AddOpSnapshot<WarpRest>(alloc_, opSnapshots, offset, shape)) {
            return abort(AbortReason::Alloc);
          }
        }
        break;
      }

      case JSOp::BindUnqualifiedGName: {
        GlobalObject* global = &script_->global();
        PropertyName* name = loc.getPropertyName(script_);
        if (JSObject* env =
                MaybeOptimizeBindUnqualifiedGlobalName(global, name)) {
          MOZ_ASSERT(env->isTenured());
          if (!AddOpSnapshot<WarpBindUnqualifiedGName>(alloc_, opSnapshots,
                                                       offset, env)) {
            return abort(AbortReason::Alloc);
          }
        } else {
          MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        }
        break;
      }

      case JSOp::PushVarEnv: {
        Rooted<VarScope*> scope(cx_, &loc.getScope(script_)->as<VarScope>());

        auto* templateObj =
            VarEnvironmentObject::createTemplateObject(cx_, scope);
        if (!templateObj) {
          return abort(AbortReason::Alloc);
        }
        MOZ_ASSERT(templateObj->isTenured());

        if (!AddOpSnapshot<WarpVarEnvironment>(alloc_, opSnapshots, offset,
                                               templateObj)) {
          return abort(AbortReason::Alloc);
        }
        break;
      }

      case JSOp::PushLexicalEnv:
      case JSOp::FreshenLexicalEnv:
      case JSOp::RecreateLexicalEnv: {
        Rooted<LexicalScope*> scope(cx_,
                                    &loc.getScope(script_)->as<LexicalScope>());

        auto* templateObj =
            BlockLexicalEnvironmentObject::createTemplateObject(cx_, scope);
        if (!templateObj) {
          return abort(AbortReason::Alloc);
        }
        MOZ_ASSERT(templateObj->isTenured());

        if (!AddOpSnapshot<WarpLexicalEnvironment>(alloc_, opSnapshots, offset,
                                                   templateObj)) {
          return abort(AbortReason::Alloc);
        }
        break;
      }

      case JSOp::PushClassBodyEnv: {
        Rooted<ClassBodyScope*> scope(
            cx_, &loc.getScope(script_)->as<ClassBodyScope>());

        auto* templateObj =
            ClassBodyLexicalEnvironmentObject::createTemplateObject(cx_, scope);
        if (!templateObj) {
          return abort(AbortReason::Alloc);
        }
        MOZ_ASSERT(templateObj->isTenured());

        if (!AddOpSnapshot<WarpClassBodyEnvironment>(alloc_, opSnapshots,
                                                     offset, templateObj)) {
          return abort(AbortReason::Alloc);
        }
        break;
      }

      case JSOp::String:
        if (!loc.atomizeString(cx_, script_)) {
          return abort(AbortReason::Alloc);
        }
        break;
      case JSOp::GetName:
      case JSOp::GetGName:
      case JSOp::GetProp:
      case JSOp::GetElem:
      case JSOp::SetProp:
      case JSOp::StrictSetProp:
      case JSOp::Call:
      case JSOp::CallContent:
      case JSOp::CallIgnoresRv:
      case JSOp::CallIter:
      case JSOp::CallContentIter:
      case JSOp::New:
      case JSOp::NewContent:
      case JSOp::SuperCall:
      case JSOp::SpreadCall:
      case JSOp::SpreadNew:
      case JSOp::SpreadSuperCall:
      case JSOp::ToNumeric:
      case JSOp::Pos:
      case JSOp::Inc:
      case JSOp::Dec:
      case JSOp::Neg:
      case JSOp::BitNot:
      case JSOp::Iter:
      case JSOp::Eq:
      case JSOp::Ne:
      case JSOp::Lt:
      case JSOp::Le:
      case JSOp::Gt:
      case JSOp::Ge:
      case JSOp::StrictEq:
      case JSOp::StrictNe:
      case JSOp::BindName:
      case JSOp::BindUnqualifiedName:
      case JSOp::GetBoundName:
      case JSOp::Add:
      case JSOp::Sub:
      case JSOp::Mul:
      case JSOp::Div:
      case JSOp::Mod:
      case JSOp::Pow:
      case JSOp::BitAnd:
      case JSOp::BitOr:
      case JSOp::BitXor:
      case JSOp::Lsh:
      case JSOp::Rsh:
      case JSOp::Ursh:
      case JSOp::In:
      case JSOp::HasOwn:
      case JSOp::CheckPrivateField:
      case JSOp::Instanceof:
      case JSOp::GetPropSuper:
      case JSOp::InitProp:
      case JSOp::InitLockedProp:
      case JSOp::InitHiddenProp:
      case JSOp::InitElem:
      case JSOp::InitHiddenElem:
      case JSOp::InitLockedElem:
      case JSOp::InitElemInc:
      case JSOp::SetName:
      case JSOp::StrictSetName:
      case JSOp::SetGName:
      case JSOp::StrictSetGName:
      case JSOp::InitGLexical:
      case JSOp::SetElem:
      case JSOp::StrictSetElem:
      case JSOp::ToPropertyKey:
      case JSOp::OptimizeSpreadCall:
      case JSOp::Typeof:
      case JSOp::TypeofExpr:
      case JSOp::TypeofEq:
      case JSOp::NewObject:
      case JSOp::NewInit:
      case JSOp::NewArray:
      case JSOp::JumpIfFalse:
      case JSOp::JumpIfTrue:
      case JSOp::And:
      case JSOp::Or:
      case JSOp::Not:
      case JSOp::CloseIter:
      case JSOp::OptimizeGetIterator:
        MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        break;

      case JSOp::Nop:
      case JSOp::NopDestructuring:
      case JSOp::NopIsAssignOp:
      case JSOp::TryDestructuring:
      case JSOp::Lineno:
      case JSOp::DebugLeaveLexicalEnv:
      case JSOp::Undefined:
      case JSOp::Void:
      case JSOp::Null:
      case JSOp::Hole:
      case JSOp::Uninitialized:
      case JSOp::IsConstructing:
      case JSOp::False:
      case JSOp::True:
      case JSOp::Zero:
      case JSOp::One:
      case JSOp::Int8:
      case JSOp::Uint16:
      case JSOp::Uint24:
      case JSOp::Int32:
      case JSOp::Double:
      case JSOp::BigInt:
      case JSOp::Symbol:
      case JSOp::Pop:
      case JSOp::PopN:
      case JSOp::Dup:
      case JSOp::Dup2:
      case JSOp::DupAt:
      case JSOp::Swap:
      case JSOp::Pick:
      case JSOp::Unpick:
      case JSOp::GetLocal:
      case JSOp::SetLocal:
      case JSOp::InitLexical:
      case JSOp::GetArg:
      case JSOp::GetFrameArg:
      case JSOp::SetArg:
      case JSOp::ArgumentsLength:
      case JSOp::GetActualArg:
      case JSOp::JumpTarget:
      case JSOp::LoopHead:
      case JSOp::Case:
      case JSOp::Default:
      case JSOp::Coalesce:
      case JSOp::Goto:
      case JSOp::DebugCheckSelfHosted:
      case JSOp::DynamicImport:
      case JSOp::ToString:
      case JSOp::GlobalOrEvalDeclInstantiation:
      case JSOp::BindVar:
      case JSOp::MutateProto:
      case JSOp::Callee:
      case JSOp::ToAsyncIter:
      case JSOp::ObjWithProto:
      case JSOp::GetAliasedVar:
      case JSOp::SetAliasedVar:
      case JSOp::InitAliasedLexical:
      case JSOp::EnvCallee:
      case JSOp::MoreIter:
      case JSOp::EndIter:
      case JSOp::IsNoIter:
      case JSOp::IsNullOrUndefined:
      case JSOp::DelProp:
      case JSOp::StrictDelProp:
      case JSOp::DelElem:
      case JSOp::StrictDelElem:
      case JSOp::SetFunName:
      case JSOp::PopLexicalEnv:
      case JSOp::ImplicitThis:
      case JSOp::CheckClassHeritage:
      case JSOp::CheckThis:
      case JSOp::CheckThisReinit:
      case JSOp::Generator:
      case JSOp::AfterYield:
      case JSOp::FinalYieldRval:
      case JSOp::AsyncResolve:
      case JSOp::AsyncReject:
      case JSOp::CheckResumeKind:
      case JSOp::CanSkipAwait:
      case JSOp::MaybeExtractAwaitValue:
      case JSOp::AsyncAwait:
      case JSOp::Await:
      case JSOp::CheckReturn:
      case JSOp::CheckLexical:
      case JSOp::CheckAliasedLexical:
      case JSOp::InitHomeObject:
      case JSOp::SuperBase:
      case JSOp::SuperFun:
      case JSOp::InitElemArray:
      case JSOp::InitPropGetter:
      case JSOp::InitPropSetter:
      case JSOp::InitHiddenPropGetter:
      case JSOp::InitHiddenPropSetter:
      case JSOp::InitElemGetter:
      case JSOp::InitElemSetter:
      case JSOp::InitHiddenElemGetter:
      case JSOp::InitHiddenElemSetter:
      case JSOp::NewTarget:
      case JSOp::Object:
      case JSOp::CallSiteObj:
      case JSOp::CheckIsObj:
      case JSOp::CheckObjCoercible:
      case JSOp::FunWithProto:
      case JSOp::Debugger:
      case JSOp::TableSwitch:
      case JSOp::Exception:
      case JSOp::ExceptionAndStack:
      case JSOp::Throw:
      case JSOp::ThrowWithStack:
      case JSOp::ThrowSetConst:
      case JSOp::SetRval:
      case JSOp::GetRval:
      case JSOp::Return:
      case JSOp::RetRval:
      case JSOp::InitialYield:
      case JSOp::Yield:
      case JSOp::ResumeKind:
      case JSOp::ThrowMsg:
      case JSOp::Try:
      case JSOp::Finally:
      case JSOp::NewPrivateName:
      case JSOp::StrictConstantEq:
      case JSOp::StrictConstantNe:
#ifdef ENABLE_EXPLICIT_RESOURCE_MANAGEMENT
      case JSOp::AddDisposable:
      case JSOp::TakeDisposeCapability:
      case JSOp::CreateSuppressedError:
#endif
        // Supported by WarpBuilder. Nothing to do.
        break;

        // Unsupported ops. Don't use a 'default' here, we want to trigger a
        // compiler warning when adding a new JSOp.
#define DEF_CASE(OP) case JSOp::OP:
        WARP_UNSUPPORTED_OPCODE_LIST(DEF_CASE)
#undef DEF_CASE
#ifdef DEBUG
        return abort(AbortReason::Disable, "Unsupported opcode: %s",
                     CodeName(op));
#else
        return abort(AbortReason::Disable, "Unsupported opcode: %u",
                     uint8_t(op));
#endif
    }
  }

  auto* scriptSnapshot = new (alloc_.fallible()) WarpScriptSnapshot(
      script_, environment, std::move(opSnapshots), moduleObject);
  if (!scriptSnapshot) {
    return abort(AbortReason::Alloc);
  }

  autoClearOpSnapshots.release();
  return scriptSnapshot;
}

static void LineNumberAndColumn(HandleScript script, BytecodeLocation loc,
                                unsigned* line,
                                JS::LimitedColumnNumberOneOrigin* column) {
#ifdef DEBUG
  *line = PCToLineNumber(script, loc.toRawBytecode(), column);
#else
  *line = script->lineno();
  *column = script->column();
#endif
}

static void MaybeSetInliningStateFromJitHints(JSContext* cx,
                                              ICFallbackStub* fallbackStub,
                                              JSScript* script,
                                              BytecodeLocation loc) {
  // Only update the state if it has already been marked as a candidate.
  if (fallbackStub->trialInliningState() != TrialInliningState::Candidate) {
    return;
  }

  // Make sure the op is inlineable.
  if (!TrialInliner::IsValidInliningOp(loc.getOp())) {
    return;
  }

  if (!cx->runtime()->jitRuntime()->hasJitHintsMap()) {
    return;
  }

  JitHintsMap* jitHints = cx->runtime()->jitRuntime()->getJitHintsMap();
  uint32_t offset = loc.bytecodeToOffset(script);

  if (jitHints->hasMonomorphicInlineHintAtOffset(script, offset)) {
    fallbackStub->setTrialInliningState(TrialInliningState::MonomorphicInlined);
  }
}

AbortReasonOr<Ok> WarpScriptOracle::maybeInlineIC(WarpOpSnapshotList& snapshots,
                                                  BytecodeLocation loc) {
  // Do one of the following:
  //
  // * If the Baseline IC has a single ICStub we can inline, add a WarpCacheIR
  //   snapshot to transpile it to MIR.
  //
  // * If that single ICStub is a call IC with a known target, instead add a
  //   WarpInline snapshot to transpile the guards to MIR and inline the target.
  //
  // * If the Baseline IC is cold (never executed), add a WarpBailout snapshot
  //   so that we can collect information in Baseline.
  //
  // * Else, don't add a snapshot and rely on WarpBuilder adding an Ion IC.

  MOZ_ASSERT(loc.opHasIC());

  // Don't create snapshots when testing ICs.
  if (JitOptions.forceInlineCaches) {
    return Ok();
  }

  ICFallbackStub* fallbackStub;
  const ICEntry& entry = getICEntryAndFallback(loc, &fallbackStub);
  ICStub* firstStub = entry.firstStub();

  uint32_t offset = loc.bytecodeToOffset(script_);

  // Set the trial inlining state directly if there is a hint cached from a
  // previous compilation.
  MaybeSetInliningStateFromJitHints(cx_, fallbackStub, script_, loc);

  // Clear the used-by-transpiler flag on the IC. It can still be set from a
  // previous compilation because we don't clear the flag on every IC when
  // invalidating.
  fallbackStub->clearUsedByTranspiler();

  if (firstStub == fallbackStub) {
    [[maybe_unused]] unsigned line;
    [[maybe_unused]] JS::LimitedColumnNumberOneOrigin column;
    LineNumberAndColumn(script_, loc, &line, &column);

    // No optimized stubs.
    JitSpew(JitSpew_WarpTranspiler,
            "fallback stub (entered-count: %" PRIu32
            ") for JSOp::%s @ %s:%u:%u",
            fallbackStub->enteredCount(), CodeName(loc.getOp()),
            script_->filename(), line, column.oneOriginValue());

    // If the fallback stub was used but there's no optimized stub, use an IC.
    if (fallbackStub->enteredCount() != 0) {
      return Ok();
    }

    // Cold IC. Bailout to collect information.
    if (!AddOpSnapshot<WarpBailout>(alloc_, snapshots, offset)) {
      return abort(AbortReason::Alloc);
    }
    return Ok();
  }

  ICCacheIRStub* stub = firstStub->toCacheIRStub();

  // Don't transpile if this IC ever encountered a case where it had
  // no stub to attach.
  if (fallbackStub->state().hasFailures()) {
    [[maybe_unused]] unsigned line;
    [[maybe_unused]] JS::LimitedColumnNumberOneOrigin column;
    LineNumberAndColumn(script_, loc, &line, &column);

    JitSpew(JitSpew_WarpTranspiler, "Failed to attach for JSOp::%s @ %s:%u:%u",
            CodeName(loc.getOp()), script_->filename(), line,
            column.oneOriginValue());
    return Ok();
  }

  // Don't transpile if there are other stubs with entered-count > 0. Counters
  // are reset when a new stub is attached so this means the stub that was added
  // most recently didn't handle all cases.
  // If this code is changed, ICScript::hash may also need changing.
  bool firstStubHandlesAllCases = true;
  for (ICStub* next = stub->next(); next; next = next->maybeNext()) {
    if (next->enteredCount() != 0) {
      firstStubHandlesAllCases = false;
      break;
    }
  }

  if (!firstStubHandlesAllCases) {
    // In some polymorphic cases, we can generate better code than the
    // default fallback if we know the observed types of the operands
    // and their relative frequency.
    if (ICSupportsPolymorphicTypeData(loc.getOp()) &&
        fallbackStub->enteredCount() == 0) {
      bool inlinedPolymorphicTypes = false;
      MOZ_TRY_VAR(
          inlinedPolymorphicTypes,
          maybeInlinePolymorphicTypes(snapshots, loc, stub, fallbackStub));
      if (inlinedPolymorphicTypes) {
        return Ok();
      }
    }

    [[maybe_unused]] unsigned line;
    [[maybe_unused]] JS::LimitedColumnNumberOneOrigin column;
    LineNumberAndColumn(script_, loc, &line, &column);

    JitSpew(JitSpew_WarpTranspiler,
            "multiple active stubs for JSOp::%s @ %s:%u:%u",
            CodeName(loc.getOp()), script_->filename(), line,
            column.oneOriginValue());
    return Ok();
  }

  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  const uint8_t* stubData = stub->stubDataStart();

  // Only create a snapshot if all opcodes are supported by the transpiler.
  CacheIRReader reader(stubInfo);
  while (reader.more()) {
    CacheOp op = reader.readOp();
    CacheIROpInfo opInfo = CacheIROpInfos[size_t(op)];
    reader.skip(opInfo.argLength);

    if (!opInfo.transpile) {
      [[maybe_unused]] unsigned line;
      [[maybe_unused]] JS::LimitedColumnNumberOneOrigin column;
      LineNumberAndColumn(script_, loc, &line, &column);

      MOZ_ASSERT(
          fallbackStub->trialInliningState() != TrialInliningState::Inlined,
          "Trial-inlined stub not supported by transpiler");

      // Unsupported CacheIR opcode.
      JitSpew(JitSpew_WarpTranspiler,
              "unsupported CacheIR opcode %s for JSOp::%s @ %s:%u:%u",
              CacheIROpNames[size_t(op)], CodeName(loc.getOp()),
              script_->filename(), line, column.oneOriginValue());
      return Ok();
    }

    // While on the main thread, ensure code stubs exist for ops that require
    // them.
    switch (op) {
      case CacheOp::CallRegExpMatcherResult:
        if (!oracle_->snapshotJitZoneStub(JitZone::StubKind::RegExpMatcher)) {
          return abort(AbortReason::Error);
        }
        break;
      case CacheOp::CallRegExpSearcherResult:
        if (!oracle_->snapshotJitZoneStub(JitZone::StubKind::RegExpSearcher)) {
          return abort(AbortReason::Error);
        }
        break;
      case CacheOp::RegExpBuiltinExecMatchResult:
        if (!oracle_->snapshotJitZoneStub(JitZone::StubKind::RegExpExecMatch)) {
          return abort(AbortReason::Error);
        }
        break;
      case CacheOp::RegExpBuiltinExecTestResult:
        if (!oracle_->snapshotJitZoneStub(JitZone::StubKind::RegExpExecTest)) {
          return abort(AbortReason::Error);
        }
        break;
      case CacheOp::ConcatStringsResult:
        if (!oracle_->snapshotJitZoneStub(JitZone::StubKind::StringConcat)) {
          return abort(AbortReason::Error);
        }
        break;
      default:
        break;
    }
  }

  // Check GC is not possible between updating stub pointers and creating the
  // snapshot.
  JS::AutoAssertNoGC nogc;

  // Copy the ICStub data to protect against the stub being unlinked or mutated.
  // We don't need to copy the CacheIRStubInfo: because we store and trace the
  // stub's JitCode*, the baselineCacheIRStubCodes_ map in JitZone will keep it
  // alive.
  uint8_t* stubDataCopy = nullptr;
  size_t bytesNeeded = stubInfo->stubDataSize();
  if (bytesNeeded > 0) {
    stubDataCopy = alloc_.allocateArray<uint8_t>(bytesNeeded);
    if (!stubDataCopy) {
      return abort(AbortReason::Alloc);
    }

    // Note: nursery pointers are handled below and the read barrier for weak
    // pointers is handled above so we can do a bitwise copy here.
    std::copy_n(stubData, bytesNeeded, stubDataCopy);

    if (!replaceNurseryAndAllocSitePointers(stub, stubInfo, stubDataCopy)) {
      return abort(AbortReason::Alloc);
    }
  }

  JitCode* jitCode = stub->jitCode();

  if (fallbackStub->trialInliningState() == TrialInliningState::Inlined ||
      fallbackStub->trialInliningState() ==
          TrialInliningState::MonomorphicInlined) {
    bool inlinedCall;
    MOZ_TRY_VAR(inlinedCall, maybeInlineCall(snapshots, loc, stub, fallbackStub,
                                             stubDataCopy));
    if (inlinedCall) {
      return Ok();
    }
  }

  if (!AddOpSnapshot<WarpCacheIR>(alloc_, snapshots, offset, jitCode, stubInfo,
                                  stubDataCopy)) {
    return abort(AbortReason::Alloc);
  }

  fallbackStub->setUsedByTranspiler();

  return Ok();
}

AbortReasonOr<bool> WarpScriptOracle::maybeInlineCall(
    WarpOpSnapshotList& snapshots, BytecodeLocation loc, ICCacheIRStub* stub,
    ICFallbackStub* fallbackStub, uint8_t* stubDataCopy) {
  Maybe<InlinableOpData> inlineData = FindInlinableOpData(stub, loc);
  if (inlineData.isNothing()) {
    return false;
  }

  RootedFunction targetFunction(cx_, inlineData->target);
  if (!TrialInliner::canInline(targetFunction, script_, loc)) {
    return false;
  }

  bool isTrialInlined =
      fallbackStub->trialInliningState() == TrialInliningState::Inlined;
  MOZ_ASSERT_IF(!isTrialInlined, fallbackStub->trialInliningState() ==
                                     TrialInliningState::MonomorphicInlined);

  RootedScript targetScript(cx_, targetFunction->nonLazyScript());
  ICScript* icScript = nullptr;
  if (isTrialInlined) {
    icScript = inlineData->icScript;
  } else {
    JitScript* jitScript = targetScript->jitScript();
    icScript = jitScript->icScript();
  }

  if (!icScript) {
    return false;
  }

  // This is just a cheap check to limit the damage we can do to ourselves if
  // we try to monomorphically inline an indirectly recursive call.
  const uint32_t maxInliningDepth = 8;
  if (!isTrialInlined &&
      info_->inlineScriptTree()->depth() > maxInliningDepth) {
    return false;
  }

  // And this is a second cheap check to ensure monomorphic inlining doesn't
  // cause us to blow past our script size budget.
  if (oracle_->accumulatedBytecodeSize() + targetScript->length() >
      JitOptions.ionMaxScriptSize) {
    return false;
  }

  // Add the inlined script to the inline script tree.
  LifoAlloc* lifoAlloc = alloc_.lifoAlloc();
  InlineScriptTree* inlineScriptTree = info_->inlineScriptTree()->addCallee(
      &alloc_, loc.toRawBytecode(), targetScript, !isTrialInlined);
  if (!inlineScriptTree) {
    return abort(AbortReason::Alloc);
  }

  // Create a CompileInfo for the inlined script.
  jsbytecode* osrPc = nullptr;
  bool needsArgsObj = targetScript->needsArgsObj();
  CompileInfo* info = lifoAlloc->new_<CompileInfo>(
      mirGen_.runtime, targetScript, targetFunction, osrPc, needsArgsObj,
      inlineScriptTree);
  if (!info) {
    return abort(AbortReason::Alloc);
  }

  // Take a snapshot of the CacheIR.
  uint32_t offset = loc.bytecodeToOffset(script_);
  JitCode* jitCode = stub->jitCode();
  const CacheIRStubInfo* stubInfo = stub->stubInfo();
  WarpCacheIR* cacheIRSnapshot = new (alloc_.fallible())
      WarpCacheIR(offset, jitCode, stubInfo, stubDataCopy);
  if (!cacheIRSnapshot) {
    return abort(AbortReason::Alloc);
  }

  // Read barrier for weak stub data copied into the snapshot.
  Zone* zone = jitCode->zone();
  if (zone->needsIncrementalBarrier()) {
    TraceWeakCacheIRStub(zone->barrierTracer(), stub, stub->stubInfo());
  }

  // Take a snapshot of the inlined script (which may do more
  // inlining recursively).
  WarpScriptOracle scriptOracle(cx_, oracle_, targetScript, info, icScript);

  AbortReasonOr<WarpScriptSnapshot*> maybeScriptSnapshot =
      scriptOracle.createScriptSnapshot();

  if (maybeScriptSnapshot.isErr()) {
    JitSpew(JitSpew_WarpTranspiler, "Can't create snapshot for JSOp::%s",
            CodeName(loc.getOp()));

    switch (maybeScriptSnapshot.unwrapErr()) {
      case AbortReason::Disable: {
        // If the target script can't be warp-compiled, mark it as
        // uninlineable, clean up, and fall through to the non-inlined path.

        // If we monomorphically inline mutually recursive functions,
        // we can reach this point more than once for the same stub.
        // We should only unlink the stub once.
        ICEntry* entry = icScript_->icEntryForStub(fallbackStub);
        MOZ_ASSERT_IF(entry->firstStub() != stub,
                      entry->firstStub() == stub->next());
        if (entry->firstStub() == stub) {
          fallbackStub->unlinkStub(cx_->zone(), entry, /*prev=*/nullptr, stub);
        }
        targetScript->setUninlineable();
        info_->inlineScriptTree()->removeCallee(inlineScriptTree);
        if (isTrialInlined) {
          icScript_->removeInlinedChild(loc.bytecodeToOffset(script_));
        }
        fallbackStub->setTrialInliningState(TrialInliningState::Failure);
        return false;
      }
      case AbortReason::Error:
      case AbortReason::Alloc:
        return Err(maybeScriptSnapshot.unwrapErr());
      default:
        MOZ_CRASH("Unexpected abort reason");
    }
  }

  WarpScriptSnapshot* scriptSnapshot = maybeScriptSnapshot.unwrap();
  oracle_->addScriptSnapshot(scriptSnapshot, icScript, targetScript->length());
#ifdef DEBUG
  if (!isTrialInlined && targetScript->jitScript()->hasPurgedStubs()) {
    oracle_->ignoreFailedICHash();
  }
#endif

  if (!AddOpSnapshot<WarpInlinedCall>(alloc_, snapshots, offset,
                                      cacheIRSnapshot, scriptSnapshot, info)) {
    return abort(AbortReason::Alloc);
  }
  fallbackStub->setUsedByTranspiler();

  // Store the location of this monomorphic inline as a hint for future
  // compilations.
  if (!isTrialInlined && cx_->runtime()->jitRuntime()->hasJitHintsMap()) {
    JitHintsMap* jitHints = cx_->runtime()->jitRuntime()->getJitHintsMap();
    if (!jitHints->addMonomorphicInlineLocation(script_, loc)) {
      return abort(AbortReason::Alloc);
    }
  }

  return true;
}

bool WarpOracle::snapshotJitZoneStub(JitZone::StubKind kind) {
  if (zoneStubs_[kind]) {
    return true;
  }
  JitCode* stub = cx_->zone()->jitZone()->ensureStubExists(cx_, kind);
  if (!stub) {
    return false;
  }
  zoneStubs_[kind] = stub;
  return true;
}

void WarpOracle::ignoreFailedICHash() {
  outerScript_->jitScript()->notePurgedStubs();
}

struct TypeFrequency {
  TypeData typeData_;
  uint32_t successCount_;
  TypeFrequency(TypeData typeData, uint32_t successCount)
      : typeData_(typeData), successCount_(successCount) {}

  // Sort highest frequency first.
  bool operator<(const TypeFrequency& other) const {
    return other.successCount_ < successCount_;
  }
};

AbortReasonOr<bool> WarpScriptOracle::maybeInlinePolymorphicTypes(
    WarpOpSnapshotList& snapshots, BytecodeLocation loc,
    ICCacheIRStub* firstStub, ICFallbackStub* fallbackStub) {
  MOZ_ASSERT(ICSupportsPolymorphicTypeData(loc.getOp()));

  // We use polymorphic type data if there are multiple active stubs,
  // all of which have type data available.
  Vector<TypeFrequency, 6, SystemAllocPolicy> candidates;
  for (ICStub* stub = firstStub; !stub->isFallback();
       stub = stub->maybeNext()) {
    ICCacheIRStub* cacheIRStub = stub->toCacheIRStub();
    uint32_t successCount =
        cacheIRStub->enteredCount() - cacheIRStub->next()->enteredCount();
    if (successCount == 0) {
      continue;
    }
    TypeData types = cacheIRStub->typeData();
    if (!types.hasData()) {
      return false;
    }
    if (!candidates.append(TypeFrequency(types, successCount))) {
      return abort(AbortReason::Alloc);
    }
  }
  if (candidates.length() < 2) {
    return false;
  }

  // Sort candidates by success frequency.
  std::sort(candidates.begin(), candidates.end());

  TypeDataList list;
  for (auto& candidate : candidates) {
    list.addTypeData(candidate.typeData_);
  }

  uint32_t offset = loc.bytecodeToOffset(script_);
  if (!AddOpSnapshot<WarpPolymorphicTypes>(alloc_, snapshots, offset, list)) {
    return abort(AbortReason::Alloc);
  }

  return true;
}

bool WarpScriptOracle::replaceNurseryAndAllocSitePointers(
    ICCacheIRStub* stub, const CacheIRStubInfo* stubInfo,
    uint8_t* stubDataCopy) {
  // If the stub data contains nursery object pointers, replace them with the
  // corresponding nursery index. See WarpObjectField.
  //
  // If the stub data contains allocation site pointers replace them with the
  // initial heap to use, because the site's state may be mutated by the main
  // thread while we are compiling.
  //
  // If the stub data contains weak pointers then trigger a read barrier. This
  // is necessary as these will now be strong references in the snapshot.
  //
  // If the stub data contains strings then atomize them. This ensures we don't
  // try to access potentially unstable characters from a background thread and
  // also facilitates certain optimizations.
  //
  // Also asserts non-object fields don't contain nursery pointers.

  uint32_t field = 0;
  size_t offset = 0;
  while (true) {
    StubField::Type fieldType = stubInfo->fieldType(field);
    switch (fieldType) {
      case StubField::Type::RawInt32:
      case StubField::Type::RawPointer:
      case StubField::Type::RawInt64:
      case StubField::Type::Double:
        break;
      case StubField::Type::Shape:
        static_assert(std::is_convertible_v<Shape*, gc::TenuredCell*>,
                      "Code assumes shapes are tenured");
        break;
      case StubField::Type::WeakShape: {
        static_assert(std::is_convertible_v<Shape*, gc::TenuredCell*>,
                      "Code assumes shapes are tenured");
        stubInfo->getStubField<StubField::Type::WeakShape>(stub, offset).get();
        break;
      }
      case StubField::Type::WeakGetterSetter: {
        static_assert(std::is_convertible_v<GetterSetter*, gc::TenuredCell*>,
                      "Code assumes GetterSetters are tenured");
        stubInfo->getStubField<StubField::Type::WeakGetterSetter>(stub, offset)
            .get();
        break;
      }
      case StubField::Type::Symbol:
        static_assert(std::is_convertible_v<JS::Symbol*, gc::TenuredCell*>,
                      "Code assumes symbols are tenured");
        break;
      case StubField::Type::WeakBaseScript: {
        static_assert(std::is_convertible_v<BaseScript*, gc::TenuredCell*>,
                      "Code assumes scripts are tenured");
        stubInfo->getStubField<StubField::Type::WeakBaseScript>(stub, offset)
            .get();
        break;
      }
      case StubField::Type::JitCode:
        static_assert(std::is_convertible_v<JitCode*, gc::TenuredCell*>,
                      "Code assumes JitCodes are tenured");
        break;
      case StubField::Type::JSObject: {
        JSObject* obj =
            stubInfo->getStubField<StubField::Type::JSObject>(stub, offset);
        if (!maybeReplaceNurseryPointer(stubInfo, stubDataCopy, obj, offset)) {
          return false;
        }
        break;
      }
      case StubField::Type::WeakObject: {
        JSObject* obj =
            stubInfo->getStubField<StubField::Type::WeakObject>(stub, offset);
        if (!maybeReplaceNurseryPointer(stubInfo, stubDataCopy, obj, offset)) {
          return false;
        }
        break;
      }
      case StubField::Type::String: {
        uintptr_t oldWord = stubInfo->getStubRawWord(stub, offset);
        JSString* str = reinterpret_cast<JSString*>(oldWord);
        MOZ_ASSERT(!IsInsideNursery(str));
        JSAtom* atom = AtomizeString(cx_, str);
        if (!atom) {
          return false;
        }
        if (atom != str) {
          uintptr_t newWord = reinterpret_cast<uintptr_t>(atom);
          stubInfo->replaceStubRawWord(stubDataCopy, offset, oldWord, newWord);
        }
        break;
      }
      case StubField::Type::Id: {
#ifdef DEBUG
        // jsid never contains nursery-allocated things.
        jsid id = stubInfo->getStubField<StubField::Type::Id>(stub, offset);
        MOZ_ASSERT_IF(id.isGCThing(),
                      !IsInsideNursery(id.toGCCellPtr().asCell()));
#endif
        break;
      }
      case StubField::Type::Value: {
        Value v =
            stubInfo->getStubField<StubField::Type::Value>(stub, offset).get();
        MOZ_ASSERT_IF(v.isGCThing(), !IsInsideNursery(v.toGCThing()));
        if (v.isString()) {
          Value newVal;
          JSAtom* atom = AtomizeString(cx_, v.toString());
          if (!atom) {
            return false;
          }
          newVal.setString(atom);
          stubInfo->replaceStubRawValueBits(stubDataCopy, offset, v.asRawBits(),
                                            newVal.asRawBits());
        }
        break;
      }
      case StubField::Type::AllocSite: {
        uintptr_t oldWord = stubInfo->getStubRawWord(stub, offset);
        auto* site = reinterpret_cast<gc::AllocSite*>(oldWord);
        gc::Heap initialHeap = site->initialHeap();
        uintptr_t newWord = uintptr_t(initialHeap);
        stubInfo->replaceStubRawWord(stubDataCopy, offset, oldWord, newWord);
        break;
      }
      case StubField::Type::Limit:
        return true;  // Done.
    }
    field++;
    offset += StubField::sizeInBytes(fieldType);
  }
}

bool WarpScriptOracle::maybeReplaceNurseryPointer(
    const CacheIRStubInfo* stubInfo, uint8_t* stubDataCopy, JSObject* obj,
    size_t offset) {
  if (!IsInsideNursery(obj)) {
    return true;
  }

  uint32_t nurseryIndex;
  if (!oracle_->registerNurseryObject(obj, &nurseryIndex)) {
    return false;
  }

  uintptr_t oldWord = WarpObjectField::fromObject(obj).rawData();
  uintptr_t newWord = WarpObjectField::fromNurseryIndex(nurseryIndex).rawData();
  stubInfo->replaceStubRawWord(stubDataCopy, offset, oldWord, newWord);
  return true;
}

bool WarpOracle::registerNurseryObject(JSObject* obj, uint32_t* nurseryIndex) {
  MOZ_ASSERT(IsInsideNursery(obj));

  auto p = nurseryObjectsMap_.lookupForAdd(obj);
  if (p) {
    *nurseryIndex = p->value();
    return true;
  }

  if (!nurseryObjects_.append(obj)) {
    return false;
  }
  *nurseryIndex = nurseryObjects_.length() - 1;
  return nurseryObjectsMap_.add(p, obj, *nurseryIndex);
}
