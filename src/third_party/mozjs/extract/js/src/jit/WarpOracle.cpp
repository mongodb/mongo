/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/WarpOracle.h"

#include "mozilla/IntegerPrintfMacros.h"
#include "mozilla/ScopeExit.h"

#include <algorithm>

#include "jit/CacheIR.h"
#include "jit/CacheIRCompiler.h"
#include "jit/CacheIROpsGenerated.h"
#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitRealm.h"
#include "jit/JitScript.h"
#include "jit/JitSpewer.h"
#include "jit/MIRGenerator.h"
#include "jit/TypeData.h"
#include "jit/WarpBuilder.h"
#include "jit/WarpCacheIRTranspiler.h"
#include "vm/BuiltinObjectKind.h"
#include "vm/BytecodeIterator.h"
#include "vm/BytecodeLocation.h"
#include "vm/GetterSetter.h"
#include "vm/Opcodes.h"

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

  AbortReasonOr<WarpEnvironment> createEnvironment();
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
  [[nodiscard]] bool replaceNurseryPointers(ICCacheIRStub* stub,
                                            const CacheIRStubInfo* stubInfo,
                                            uint8_t* stubDataCopy);

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

void WarpOracle::addScriptSnapshot(WarpScriptSnapshot* scriptSnapshot) {
  scriptSnapshots_.insertBack(scriptSnapshot);
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
          outerScript_->column(), static_cast<JSScript*>(outerScript_),
          outerScript_->getWarmUpCount(),
          outerScript_->isGenerator() ? " isGenerator" : "",
          outerScript_->isAsync() ? " isAsync" : "");
#endif

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
      WarpSnapshot(cx_, alloc_, std::move(scriptSnapshots_), bailoutInfo_,
                   recordFinalWarmUpCount);
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
  // have a TranspiledCacheIR bailout, and assert that the hash has
  // changed when we recompile.
  //
  // Note: this assertion catches potential performance issues.
  // Failing this assertion is not a correctness/security problem.
  // We therefore ignore cases involving OOM, stack overflow, or
  // stubs purged by GC.
  HashNumber hash = icScript->hash();
  if (outerScript_->jitScript()->hasFailedICHash()) {
    HashNumber oldHash = outerScript_->jitScript()->getFailedICHash();
    MOZ_ASSERT_IF(hash == oldHash, cx_->hadNondeterministicException());
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

AbortReasonOr<WarpEnvironment> WarpScriptOracle::createEnvironment() {
  // Don't do anything if the script doesn't use the environment chain.
  // Always make an environment chain if the script needs an arguments object
  // because ArgumentsObject construction requires the environment chain to be
  // passed in.
  if (!script_->jitScript()->usesEnvironmentChain() &&
      !script_->needsArgsObj()) {
    return WarpEnvironment(NoEnvironment());
  }

  if (ModuleObject* module = script_->module()) {
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

  // Parameter expression-induced extra var environment not yet handled.
  if (fun->needsExtraBodyVarEnvironment()) {
    return abort(AbortReason::Disable, "Extra var environment unsupported");
  }

  JSObject* templateEnv = script_->jitScript()->templateEnvironment();

  CallObject* callObjectTemplate = nullptr;
  if (fun->needsCallObject()) {
    callObjectTemplate = &templateEnv->as<CallObject>();
  }

  NamedLambdaObject* namedLambdaTemplate = nullptr;
  if (fun->needsNamedLambdaEnvironment()) {
    if (callObjectTemplate) {
      templateEnv = templateEnv->enclosingEnvironment();
    }
    namedLambdaTemplate = &templateEnv->as<NamedLambdaObject>();
  }

  return WarpEnvironment(
      FunctionEnvironment(callObjectTemplate, namedLambdaTemplate));
}

AbortReasonOr<WarpScriptSnapshot*> WarpScriptOracle::createScriptSnapshot() {
  MOZ_ASSERT(script_->hasJitScript());

  if (!script_->jitScript()->ensureHasCachedIonData(cx_, script_)) {
    return abort(AbortReason::Error);
  }

  if (script_->jitScript()->hasTryFinally()) {
    return abort(AbortReason::Disable, "Try-finally not supported");
  }

  if (script_->failedBoundsCheck()) {
    oracle_->bailoutInfo().setFailedBoundsCheck();
  }
  if (script_->failedLexicalCheck()) {
    oracle_->bailoutInfo().setFailedLexicalCheck();
  }

  WarpEnvironment environment{NoEnvironment()};
  MOZ_TRY_VAR(environment, createEnvironment());

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
            script_->realm()->maybeArgumentsTemplateObject(mapped);
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
        if (script_->hasNonSyntacticScope()) {
          // We don't compile global scripts with a non-syntactic scope, but
          // we can end up here when we're compiling an arrow function.
          return abort(AbortReason::Disable,
                       "JSOp::GlobalThis with non-syntactic scope");
        }
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
        PropertyName* name = loc.getPropertyName(script_);
        Value val;
        if (cx_->global()->maybeExistingIntrinsicValue(name, &val)) {
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

      case JSOp::CallSiteObj: {
        // Prepare the object so that WarpBuilder can just push it as constant.
        if (!ProcessCallSiteObjOperation(cx_, script_, loc.toRawBytecode())) {
          return abort(AbortReason::Error);
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

      case JSOp::Lambda:
      case JSOp::LambdaArrow: {
        JSFunction* fun = loc.getFunction(script_);
        if (IsAsmJSModule(fun)) {
          return abort(AbortReason::Disable, "asm.js module function lambda");
        }

        if (!AddOpSnapshot<WarpLambda>(alloc_, opSnapshots, offset,
                                       fun->baseScript(), fun->flags(),
                                       fun->nargs())) {
          return abort(AbortReason::Alloc);
        }
        break;
      }

      case JSOp::GetElemSuper: {
#if defined(JS_CODEGEN_X86)
        // x86 does not have enough registers if profiling is enabled.
        if (mirGen_.instrumentedProfiling()) {
          return abort(AbortReason::Disable,
                       "GetElemSuper with profiling is not supported on x86");
        }
#endif
        MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        break;
      }

      case JSOp::Rest: {
        if (Shape* shape = script_->global().maybeArrayShape()) {
          if (!AddOpSnapshot<WarpRest>(alloc_, opSnapshots, offset, shape)) {
            return abort(AbortReason::Alloc);
          }
        }
        break;
      }

      case JSOp::BindGName: {
        RootedGlobalObject global(cx_, &script_->global());
        RootedPropertyName name(cx_, loc.getPropertyName(script_));
        if (JSObject* env = MaybeOptimizeBindGlobalName(cx_, global, name)) {
          MOZ_ASSERT(env->isTenured());
          if (!AddOpSnapshot<WarpBindGName>(alloc_, opSnapshots, offset, env)) {
            return abort(AbortReason::Alloc);
          }
        } else {
          MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        }
        break;
      }

      case JSOp::GetName:
      case JSOp::GetGName:
      case JSOp::GetProp:
      case JSOp::GetElem:
      case JSOp::SetProp:
      case JSOp::StrictSetProp:
      case JSOp::Call:
      case JSOp::CallIgnoresRv:
      case JSOp::CallIter:
      case JSOp::FunCall:
      case JSOp::FunApply:
      case JSOp::New:
      case JSOp::SuperCall:
      case JSOp::SpreadCall:
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
      case JSOp::NewObject:
      case JSOp::NewInit:
      case JSOp::NewArray:
      case JSOp::JumpIfFalse:
      case JSOp::JumpIfTrue:
      case JSOp::And:
      case JSOp::Or:
      case JSOp::Not:
        MOZ_TRY(maybeInlineIC(opSnapshots, loc));
        break;

      case JSOp::Nop:
      case JSOp::NopDestructuring:
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
      case JSOp::ResumeIndex:
      case JSOp::BigInt:
      case JSOp::String:
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
      case JSOp::SetArg:
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
      case JSOp::DelProp:
      case JSOp::StrictDelProp:
      case JSOp::DelElem:
      case JSOp::StrictDelElem:
      case JSOp::SetFunName:
      case JSOp::PushLexicalEnv:
      case JSOp::PopLexicalEnv:
      case JSOp::FreshenLexicalEnv:
      case JSOp::RecreateLexicalEnv:
      case JSOp::PushClassBodyEnv:
      case JSOp::ImplicitThis:
      case JSOp::GImplicitThis:
      case JSOp::CheckClassHeritage:
      case JSOp::CheckThis:
      case JSOp::CheckThisReinit:
      case JSOp::Generator:
      case JSOp::AfterYield:
      case JSOp::FinalYieldRval:
      case JSOp::AsyncResolve:
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
      case JSOp::CheckIsObj:
      case JSOp::CheckObjCoercible:
      case JSOp::FunWithProto:
      case JSOp::SpreadNew:
      case JSOp::SpreadSuperCall:
      case JSOp::Debugger:
      case JSOp::TableSwitch:
      case JSOp::Exception:
      case JSOp::Throw:
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
                                unsigned* line, unsigned* column) {
#ifdef DEBUG
  *line = PCToLineNumber(script, loc.toRawBytecode(), column);
#else
  *line = script->lineno();
  *column = script->column();
#endif
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

  // Clear the used-by-transpiler flag on the IC. It can still be set from a
  // previous compilation because we don't clear the flag on every IC when
  // invalidating.
  fallbackStub->clearUsedByTranspiler();

  if (firstStub == fallbackStub) {
    [[maybe_unused]] unsigned line, column;
    LineNumberAndColumn(script_, loc, &line, &column);

    // No optimized stubs.
    JitSpew(JitSpew_WarpTranspiler,
            "fallback stub (entered-count: %" PRIu32
            ") for JSOp::%s @ %s:%u:%u",
            fallbackStub->enteredCount(), CodeName(loc.getOp()),
            script_->filename(), line, column);

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

    [[maybe_unused]] unsigned line, column;
    LineNumberAndColumn(script_, loc, &line, &column);

    JitSpew(JitSpew_WarpTranspiler,
            "multiple active stubs for JSOp::%s @ %s:%u:%u",
            CodeName(loc.getOp()), script_->filename(), line, column);
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
      [[maybe_unused]] unsigned line, column;
      LineNumberAndColumn(script_, loc, &line, &column);

      MOZ_ASSERT(
          fallbackStub->trialInliningState() != TrialInliningState::Inlined,
          "Trial-inlined stub not supported by transpiler");

      // Unsupported CacheIR opcode.
      JitSpew(JitSpew_WarpTranspiler,
              "unsupported CacheIR opcode %s for JSOp::%s @ %s:%u:%u",
              CacheIROpNames[size_t(op)], CodeName(loc.getOp()),
              script_->filename(), line, column);
      return Ok();
    }

    // While on the main thread, ensure code stubs exist for ops that require
    // them.
    switch (op) {
      case CacheOp::CallRegExpMatcherResult:
        if (!cx_->realm()->jitRealm()->ensureRegExpMatcherStubExists(cx_)) {
          return abort(AbortReason::Error);
        }
        break;
      case CacheOp::CallRegExpSearcherResult:
        if (!cx_->realm()->jitRealm()->ensureRegExpSearcherStubExists(cx_)) {
          return abort(AbortReason::Error);
        }
        break;
      case CacheOp::CallRegExpTesterResult:
        if (!cx_->realm()->jitRealm()->ensureRegExpTesterStubExists(cx_)) {
          return abort(AbortReason::Error);
        }
        break;
      default:
        break;
    }
  }

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

    // Note: nursery pointers are handled below so we don't need to trigger any
    // GC barriers and can do a bitwise copy.
    std::copy_n(stubData, bytesNeeded, stubDataCopy);

    if (!replaceNurseryPointers(stub, stubInfo, stubDataCopy)) {
      return abort(AbortReason::Alloc);
    }
  }

  JitCode* jitCode = stub->jitCode();

  if (fallbackStub->trialInliningState() == TrialInliningState::Inlined) {
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
  if (inlineData.isNothing() || !inlineData->icScript) {
    return false;
  }

  RootedFunction targetFunction(cx_, inlineData->target);
  if (!TrialInliner::canInline(targetFunction, script_, loc)) {
    return false;
  }

  RootedScript targetScript(cx_, targetFunction->nonLazyScript());
  ICScript* icScript = inlineData->icScript;

  // Add the inlined script to the inline script tree.
  LifoAlloc* lifoAlloc = alloc_.lifoAlloc();
  InlineScriptTree* inlineScriptTree = info_->inlineScriptTree()->addCallee(
      &alloc_, loc.toRawBytecode(), targetScript);
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
        ICEntry* entry = icScript_->icEntryForStub(fallbackStub);
        fallbackStub->setTrialInliningState(TrialInliningState::Failure);
        fallbackStub->unlinkStub(cx_->zone(), entry, /*prev=*/nullptr, stub);
        targetScript->setUninlineable();
        info_->inlineScriptTree()->removeCallee(inlineScriptTree);
        icScript_->removeInlinedChild(loc.bytecodeToOffset(script_));
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
  oracle_->addScriptSnapshot(scriptSnapshot);

  if (!AddOpSnapshot<WarpInlinedCall>(alloc_, snapshots, offset,
                                      cacheIRSnapshot, scriptSnapshot, info)) {
    return abort(AbortReason::Alloc);
  }
  fallbackStub->setUsedByTranspiler();
  return true;
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

bool WarpScriptOracle::replaceNurseryPointers(ICCacheIRStub* stub,
                                              const CacheIRStubInfo* stubInfo,
                                              uint8_t* stubDataCopy) {
  // If the stub data contains nursery object pointers, replace them with the
  // corresponding nursery index. See WarpObjectField.
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
      case StubField::Type::AllocSite:
        break;
      case StubField::Type::Shape:
        static_assert(std::is_convertible_v<Shape*, gc::TenuredCell*>,
                      "Code assumes shapes are tenured");
        break;
      case StubField::Type::GetterSetter:
        static_assert(std::is_convertible_v<GetterSetter*, gc::TenuredCell*>,
                      "Code assumes GetterSetters are tenured");
        break;
      case StubField::Type::Symbol:
        static_assert(std::is_convertible_v<JS::Symbol*, gc::TenuredCell*>,
                      "Code assumes symbols are tenured");
        break;
      case StubField::Type::BaseScript:
        static_assert(std::is_convertible_v<BaseScript*, gc::TenuredCell*>,
                      "Code assumes scripts are tenured");
        break;
      case StubField::Type::JSObject: {
        JSObject* obj =
            stubInfo->getStubField<ICCacheIRStub, JSObject*>(stub, offset);
        if (IsInsideNursery(obj)) {
          uint32_t nurseryIndex;
          if (!oracle_->registerNurseryObject(obj, &nurseryIndex)) {
            return false;
          }
          uintptr_t oldWord = WarpObjectField::fromObject(obj).rawData();
          uintptr_t newWord =
              WarpObjectField::fromNurseryIndex(nurseryIndex).rawData();
          stubInfo->replaceStubRawWord(stubDataCopy, offset, oldWord, newWord);
        }
        break;
      }
      case StubField::Type::String: {
#ifdef DEBUG
        JSString* str =
            stubInfo->getStubField<ICCacheIRStub, JSString*>(stub, offset);
        MOZ_ASSERT(!IsInsideNursery(str));
#endif
        break;
      }
      case StubField::Type::Id: {
#ifdef DEBUG
        // jsid never contains nursery-allocated things.
        jsid id = stubInfo->getStubField<ICCacheIRStub, jsid>(stub, offset);
        MOZ_ASSERT_IF(id.isGCThing(),
                      !IsInsideNursery(id.toGCCellPtr().asCell()));
#endif
        break;
      }
      case StubField::Type::Value: {
#ifdef DEBUG
        Value v =
            stubInfo->getStubField<ICCacheIRStub, JS::Value>(stub, offset);
        MOZ_ASSERT_IF(v.isGCThing(), !IsInsideNursery(v.toGCThing()));
#endif
        break;
      }
      case StubField::Type::Limit:
        return true;  // Done.
    }
    field++;
    offset += StubField::sizeInBytes(fieldType);
  }
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
