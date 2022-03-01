/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Stack-inl.h"

#include "mozilla/Maybe.h"  // mozilla::Maybe

#include <algorithm>  // std::max
#include <iterator>   // std::size
#include <stddef.h>   // size_t
#include <stdint.h>   // uint8_t, uint32_t
#include <utility>    // std::move

#include "debugger/DebugAPI.h"
#include "gc/Marking.h"
#include "gc/Tracer.h"  // js::TraceRoot
#include "jit/JitcodeMap.h"
#include "jit/JitRuntime.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Value.h"                 // JS::Value
#include "vm/FrameIter.h"             // js::FrameIter
#include "vm/JSContext.h"
#include "vm/Opcodes.h"
#include "wasm/WasmInstance.h"

#include "jit/JSJitFrameIter-inl.h"
#include "vm/Compartment-inl.h"
#include "vm/EnvironmentObject-inl.h"
#include "vm/Interpreter-inl.h"
#include "vm/Probes-inl.h"

using namespace js;

using mozilla::Maybe;

using JS::Value;

/*****************************************************************************/

void InterpreterFrame::initExecuteFrame(JSContext* cx, HandleScript script,
                                        AbstractFramePtr evalInFramePrev,
                                        HandleValue newTargetValue,
                                        HandleObject envChain) {
  flags_ = 0;
  script_ = script;

  Value* dstvp = (Value*)this - 1;
  dstvp[0] = newTargetValue;

  envChain_ = envChain.get();
  prev_ = nullptr;
  prevpc_ = nullptr;
  prevsp_ = nullptr;

  evalInFramePrev_ = evalInFramePrev;
  MOZ_ASSERT_IF(evalInFramePrev, isDebuggerEvalFrame());

  if (script->isDebuggee()) {
    setIsDebuggee();
  }

#ifdef DEBUG
  Debug_SetValueRangeToCrashOnTouch(&rval_, 1);
#endif
}

ArrayObject* InterpreterFrame::createRestParameter(JSContext* cx) {
  MOZ_ASSERT(script()->hasRest());
  unsigned nformal = callee().nargs() - 1, nactual = numActualArgs();
  unsigned nrest = (nactual > nformal) ? nactual - nformal : 0;
  Value* restvp = argv() + nformal;
  return NewDenseCopiedArray(cx, nrest, restvp);
}

static inline void AssertScopeMatchesEnvironment(Scope* scope,
                                                 JSObject* originalEnv) {
#ifdef DEBUG
  JSObject* env = originalEnv;
  for (ScopeIter si(scope); si; si++) {
    if (si.kind() == ScopeKind::NonSyntactic) {
      while (env->is<WithEnvironmentObject>() ||
             env->is<NonSyntacticVariablesObject>() ||
             (env->is<LexicalEnvironmentObject>() &&
              !env->as<LexicalEnvironmentObject>().isSyntactic())) {
        MOZ_ASSERT(!IsSyntacticEnvironment(env));
        env = &env->as<EnvironmentObject>().enclosingEnvironment();
      }
    } else if (si.hasSyntacticEnvironment()) {
      switch (si.kind()) {
        case ScopeKind::Function:
          MOZ_ASSERT(env->as<CallObject>()
                         .callee()
                         .maybeCanonicalFunction()
                         ->nonLazyScript() ==
                     si.scope()->as<FunctionScope>().script());
          env = &env->as<CallObject>().enclosingEnvironment();
          break;

        case ScopeKind::FunctionBodyVar:
          MOZ_ASSERT(&env->as<VarEnvironmentObject>().scope() == si.scope());
          env = &env->as<VarEnvironmentObject>().enclosingEnvironment();
          break;

        case ScopeKind::Lexical:
        case ScopeKind::SimpleCatch:
        case ScopeKind::Catch:
        case ScopeKind::NamedLambda:
        case ScopeKind::StrictNamedLambda:
        case ScopeKind::FunctionLexical:
        case ScopeKind::ClassBody:
          MOZ_ASSERT(&env->as<ScopedLexicalEnvironmentObject>().scope() ==
                     si.scope());
          env =
              &env->as<ScopedLexicalEnvironmentObject>().enclosingEnvironment();
          break;

        case ScopeKind::With:
          MOZ_ASSERT(&env->as<WithEnvironmentObject>().scope() == si.scope());
          env = &env->as<WithEnvironmentObject>().enclosingEnvironment();
          break;

        case ScopeKind::Eval:
        case ScopeKind::StrictEval:
          env = &env->as<VarEnvironmentObject>().enclosingEnvironment();
          break;

        case ScopeKind::Global:
          env =
              &env->as<GlobalLexicalEnvironmentObject>().enclosingEnvironment();
          MOZ_ASSERT(env->is<GlobalObject>());
          break;

        case ScopeKind::NonSyntactic:
          MOZ_CRASH("NonSyntactic should not have a syntactic environment");
          break;

        case ScopeKind::Module:
          MOZ_ASSERT(&env->as<ModuleEnvironmentObject>().module() ==
                     si.scope()->as<ModuleScope>().module());
          env = &env->as<ModuleEnvironmentObject>().enclosingEnvironment();
          break;

        case ScopeKind::WasmInstance:
          env =
              &env->as<WasmInstanceEnvironmentObject>().enclosingEnvironment();
          break;

        case ScopeKind::WasmFunction:
          env = &env->as<WasmFunctionCallObject>().enclosingEnvironment();
          break;
      }
    }
  }

  // In the case of a non-syntactic env chain, the immediate parent of the
  // outermost non-syntactic env may be the global lexical env, or, if
  // called from Debugger, a DebugEnvironmentProxy.
  //
  // In the case of a syntactic env chain, the outermost env is always a
  // GlobalObject.
  MOZ_ASSERT(env->is<GlobalObject>() || IsGlobalLexicalEnvironment(env) ||
             env->is<DebugEnvironmentProxy>());
#endif
}

static inline void AssertScopeMatchesEnvironment(InterpreterFrame* fp,
                                                 jsbytecode* pc) {
#ifdef DEBUG
  // If we OOMed before fully initializing the environment chain, the scope
  // and environment will definitely mismatch.
  if (fp->script()->initialEnvironmentShape() && fp->hasInitialEnvironment()) {
    AssertScopeMatchesEnvironment(fp->script()->innermostScope(pc),
                                  fp->environmentChain());
  }
#endif
}

bool InterpreterFrame::initFunctionEnvironmentObjects(JSContext* cx) {
  return js::InitFunctionEnvironmentObjects(cx, this);
}

bool InterpreterFrame::prologue(JSContext* cx) {
  RootedScript script(cx, this->script());

  MOZ_ASSERT(cx->interpreterRegs().pc == script->code());
  MOZ_ASSERT(cx->realm() == script->realm());

  if (!isFunctionFrame()) {
    return probes::EnterScript(cx, script, nullptr, this);
  }

  // At this point, we've yet to push any environments. Check that they
  // match the enclosing scope.
  AssertScopeMatchesEnvironment(script->enclosingScope(), environmentChain());

  if (callee().needsFunctionEnvironmentObjects() &&
      !initFunctionEnvironmentObjects(cx)) {
    return false;
  }

  MOZ_ASSERT_IF(isConstructing(),
                thisArgument().isObject() ||
                    thisArgument().isMagic(JS_UNINITIALIZED_LEXICAL));

  return probes::EnterScript(cx, script, script->function(), this);
}

void InterpreterFrame::epilogue(JSContext* cx, jsbytecode* pc) {
  RootedScript script(cx, this->script());
  MOZ_ASSERT(cx->realm() == script->realm());
  probes::ExitScript(cx, script, script->function(),
                     hasPushedGeckoProfilerFrame());

  // Check that the scope matches the environment at the point of leaving
  // the frame.
  AssertScopeMatchesEnvironment(this, pc);

  EnvironmentIter ei(cx, this, pc);
  UnwindAllEnvironmentsInFrame(cx, ei);

  if (isFunctionFrame()) {
    if (!callee().isGenerator() && !callee().isAsync() && isConstructing() &&
        thisArgument().isObject() && returnValue().isPrimitive()) {
      setReturnValue(thisArgument());
    }

    return;
  }

  MOZ_ASSERT(isEvalFrame() || isGlobalFrame() || isModuleFrame());
}

bool InterpreterFrame::checkReturn(JSContext* cx, HandleValue thisv) {
  MOZ_ASSERT(script()->isDerivedClassConstructor());
  MOZ_ASSERT(isFunctionFrame());
  MOZ_ASSERT(callee().isClassConstructor());

  HandleValue retVal = returnValue();
  if (retVal.isObject()) {
    return true;
  }

  if (!retVal.isUndefined()) {
    ReportValueError(cx, JSMSG_BAD_DERIVED_RETURN, JSDVG_IGNORE_STACK, retVal,
                     nullptr);
    return false;
  }

  if (thisv.isMagic(JS_UNINITIALIZED_LEXICAL)) {
    return ThrowUninitializedThis(cx);
  }

  setReturnValue(thisv);
  return true;
}

bool InterpreterFrame::pushVarEnvironment(JSContext* cx, HandleScope scope) {
  return js::PushVarEnvironmentObject(cx, scope, this);
}

bool InterpreterFrame::pushLexicalEnvironment(JSContext* cx,
                                              Handle<LexicalScope*> scope) {
  BlockLexicalEnvironmentObject* env =
      BlockLexicalEnvironmentObject::createForFrame(cx, scope, this);
  if (!env) {
    return false;
  }

  pushOnEnvironmentChain(*env);
  return true;
}

bool InterpreterFrame::freshenLexicalEnvironment(JSContext* cx) {
  Rooted<BlockLexicalEnvironmentObject*> env(
      cx, &envChain_->as<BlockLexicalEnvironmentObject>());
  BlockLexicalEnvironmentObject* fresh =
      BlockLexicalEnvironmentObject::clone(cx, env);
  if (!fresh) {
    return false;
  }

  replaceInnermostEnvironment(*fresh);
  return true;
}

bool InterpreterFrame::recreateLexicalEnvironment(JSContext* cx) {
  Rooted<BlockLexicalEnvironmentObject*> env(
      cx, &envChain_->as<BlockLexicalEnvironmentObject>());
  BlockLexicalEnvironmentObject* fresh =
      BlockLexicalEnvironmentObject::recreate(cx, env);
  if (!fresh) {
    return false;
  }

  replaceInnermostEnvironment(*fresh);
  return true;
}

bool InterpreterFrame::pushClassBodyEnvironment(JSContext* cx,
                                                Handle<ClassBodyScope*> scope) {
  ClassBodyLexicalEnvironmentObject* env =
      ClassBodyLexicalEnvironmentObject::createForFrame(cx, scope, this);
  if (!env) {
    return false;
  }

  pushOnEnvironmentChain(*env);
  return true;
}

void InterpreterFrame::trace(JSTracer* trc, Value* sp, jsbytecode* pc) {
  TraceRoot(trc, &envChain_, "env chain");
  TraceRoot(trc, &script_, "script");

  if (flags_ & HAS_ARGS_OBJ) {
    TraceRoot(trc, &argsObj_, "arguments");
  }

  if (hasReturnValue()) {
    TraceRoot(trc, &rval_, "rval");
  }

  MOZ_ASSERT(sp >= slots());

  if (hasArgs()) {
    // Trace the callee and |this|. When we're doing a moving GC, we
    // need to fix up the callee pointer before we use it below, under
    // numFormalArgs() and script().
    TraceRootRange(trc, 2, argv_ - 2, "fp callee and this");

    // Trace arguments.
    unsigned argc = std::max(numActualArgs(), numFormalArgs());
    TraceRootRange(trc, argc + isConstructing(), argv_, "fp argv");
  } else {
    // Trace newTarget.
    TraceRoot(trc, ((Value*)this) - 1, "stack newTarget");
  }

  JSScript* script = this->script();
  size_t nfixed = script->nfixed();
  size_t nlivefixed = script->calculateLiveFixed(pc);

  if (nfixed == nlivefixed) {
    // All locals are live.
    traceValues(trc, 0, sp - slots());
  } else {
    // Trace operand stack.
    traceValues(trc, nfixed, sp - slots());

    // Clear dead block-scoped locals.
    while (nfixed > nlivefixed) {
      unaliasedLocal(--nfixed).setUndefined();
    }

    // Trace live locals.
    traceValues(trc, 0, nlivefixed);
  }

  if (auto* debugEnvs = script->realm()->debugEnvs()) {
    debugEnvs->traceLiveFrame(trc, this);
  }
}

void InterpreterFrame::traceValues(JSTracer* trc, unsigned start,
                                   unsigned end) {
  if (start < end) {
    TraceRootRange(trc, end - start, slots() + start, "vm_stack");
  }
}

static void TraceInterpreterActivation(JSTracer* trc,
                                       InterpreterActivation* act) {
  for (InterpreterFrameIterator frames(act); !frames.done(); ++frames) {
    InterpreterFrame* fp = frames.frame();
    fp->trace(trc, frames.sp(), frames.pc());
  }
}

void js::TraceInterpreterActivations(JSContext* cx, JSTracer* trc) {
  for (ActivationIterator iter(cx); !iter.done(); ++iter) {
    Activation* act = iter.activation();
    if (act->isInterpreter()) {
      TraceInterpreterActivation(trc, act->asInterpreter());
    }
  }
}

/*****************************************************************************/

// Unlike the other methods of this class, this method is defined here so that
// we don't have to #include jsautooplen.h in vm/Stack.h.
void InterpreterRegs::setToEndOfScript() { sp = fp()->base(); }

/*****************************************************************************/

InterpreterFrame* InterpreterStack::pushInvokeFrame(
    JSContext* cx, const CallArgs& args, MaybeConstruct constructing) {
  LifoAlloc::Mark mark = allocator_.mark();

  RootedFunction fun(cx, &args.callee().as<JSFunction>());
  RootedScript script(cx, fun->nonLazyScript());

  Value* argv;
  InterpreterFrame* fp = getCallFrame(cx, args, script, constructing, &argv);
  if (!fp) {
    return nullptr;
  }

  fp->mark_ = mark;
  fp->initCallFrame(nullptr, nullptr, nullptr, *fun, script, argv,
                    args.length(), constructing);
  return fp;
}

InterpreterFrame* InterpreterStack::pushExecuteFrame(
    JSContext* cx, HandleScript script, HandleValue newTargetValue,
    HandleObject envChain, AbstractFramePtr evalInFrame) {
  LifoAlloc::Mark mark = allocator_.mark();

  unsigned nvars = 1 /* newTarget */ + script->nslots();
  uint8_t* buffer =
      allocateFrame(cx, sizeof(InterpreterFrame) + nvars * sizeof(Value));
  if (!buffer) {
    return nullptr;
  }

  InterpreterFrame* fp =
      reinterpret_cast<InterpreterFrame*>(buffer + 1 * sizeof(Value));
  fp->mark_ = mark;
  fp->initExecuteFrame(cx, script, evalInFrame, newTargetValue, envChain);
  fp->initLocals();

  return fp;
}

/*****************************************************************************/

InterpreterFrameIterator& InterpreterFrameIterator::operator++() {
  MOZ_ASSERT(!done());
  if (fp_ != activation_->entryFrame_) {
    pc_ = fp_->prevpc();
    sp_ = fp_->prevsp();
    fp_ = fp_->prev();
  } else {
    pc_ = nullptr;
    sp_ = nullptr;
    fp_ = nullptr;
  }
  return *this;
}

JS::ProfilingFrameIterator::ProfilingFrameIterator(
    JSContext* cx, const RegisterState& state,
    const Maybe<uint64_t>& samplePositionInProfilerBuffer)
    : cx_(cx),
      samplePositionInProfilerBuffer_(samplePositionInProfilerBuffer),
      activation_(nullptr) {
  if (!cx->runtime()->geckoProfiler().enabled()) {
    MOZ_CRASH(
        "ProfilingFrameIterator called when geckoProfiler not enabled for "
        "runtime.");
  }

  if (!cx->profilingActivation()) {
    return;
  }

  // If profiler sampling is not enabled, skip.
  if (!cx->isProfilerSamplingEnabled()) {
    return;
  }

  activation_ = cx->profilingActivation();

  MOZ_ASSERT(activation_->isProfiling());

  static_assert(sizeof(wasm::ProfilingFrameIterator) <= StorageSpace &&
                    sizeof(jit::JSJitProfilingFrameIterator) <= StorageSpace,
                "ProfilingFrameIterator::storage_ is too small");
  static_assert(alignof(void*) >= alignof(wasm::ProfilingFrameIterator) &&
                    alignof(void*) >= alignof(jit::JSJitProfilingFrameIterator),
                "ProfilingFrameIterator::storage_ is too weakly aligned");

  iteratorConstruct(state);
  settle();
}

JS::ProfilingFrameIterator::~ProfilingFrameIterator() {
  if (!done()) {
    MOZ_ASSERT(activation_->isProfiling());
    iteratorDestroy();
  }
}

void JS::ProfilingFrameIterator::operator++() {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(activation_->isJit());
  if (isWasm()) {
    ++wasmIter();
  } else {
    ++jsJitIter();
  }
  settle();
}

void JS::ProfilingFrameIterator::settleFrames() {
  // Handle transition frames (see comment in JitFrameIter::operator++).
  if (isJSJit() && !jsJitIter().done() &&
      jsJitIter().frameType() == jit::FrameType::WasmToJSJit) {
    wasm::Frame* fp = (wasm::Frame*)jsJitIter().fp();
    iteratorDestroy();
    new (storage()) wasm::ProfilingFrameIterator(fp);
    kind_ = Kind::Wasm;
    MOZ_ASSERT(!wasmIter().done());
    return;
  }

  if (isWasm() && wasmIter().done() && wasmIter().unwoundIonCallerFP()) {
    uint8_t* fp = wasmIter().unwoundIonCallerFP();
    iteratorDestroy();
    // Using this ctor will skip the first ion->wasm frame, which is
    // needed because the profiling iterator doesn't know how to unwind
    // when the callee has no script.
    new (storage())
        jit::JSJitProfilingFrameIterator((jit::CommonFrameLayout*)fp);
    kind_ = Kind::JSJit;
    MOZ_ASSERT(!jsJitIter().done());
    return;
  }
}

void JS::ProfilingFrameIterator::settle() {
  settleFrames();
  while (iteratorDone()) {
    iteratorDestroy();
    activation_ = activation_->prevProfiling();
    if (!activation_) {
      return;
    }
    iteratorConstruct();
    settleFrames();
  }
}

void JS::ProfilingFrameIterator::iteratorConstruct(const RegisterState& state) {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(activation_->isJit());

  jit::JitActivation* activation = activation_->asJit();

  // We want to know if we should start with a wasm profiling frame iterator
  // or not. To determine this, there are three possibilities:
  // - we've exited to C++ from wasm, in which case the activation
  //   exitFP low bit is tagged and we can test hasWasmExitFP().
  // - we're in wasm code, so we can do a lookup on PC.
  // - in all the other cases, we're not in wasm or we haven't exited from
  //   wasm.
  if (activation->hasWasmExitFP() || wasm::InCompiledCode(state.pc)) {
    new (storage()) wasm::ProfilingFrameIterator(*activation, state);
    kind_ = Kind::Wasm;
    return;
  }

  new (storage()) jit::JSJitProfilingFrameIterator(cx_, state.pc);
  kind_ = Kind::JSJit;
}

void JS::ProfilingFrameIterator::iteratorConstruct() {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(activation_->isJit());

  jit::JitActivation* activation = activation_->asJit();

  // The same reasoning as in the above iteratorConstruct variant applies
  // here, except that it's even simpler: since this activation is higher up
  // on the stack, it can only have exited to C++, through wasm or ion.
  if (activation->hasWasmExitFP()) {
    new (storage()) wasm::ProfilingFrameIterator(*activation);
    kind_ = Kind::Wasm;
    return;
  }

  auto* fp = (jit::ExitFrameLayout*)activation->jsExitFP();
  new (storage()) jit::JSJitProfilingFrameIterator(fp);
  kind_ = Kind::JSJit;
}

void JS::ProfilingFrameIterator::iteratorDestroy() {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(activation_->isJit());

  if (isWasm()) {
    wasmIter().~ProfilingFrameIterator();
    return;
  }

  jsJitIter().~JSJitProfilingFrameIterator();
}

bool JS::ProfilingFrameIterator::iteratorDone() {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(activation_->isJit());

  if (isWasm()) {
    return wasmIter().done();
  }

  return jsJitIter().done();
}

void* JS::ProfilingFrameIterator::stackAddress() const {
  MOZ_ASSERT(!done());
  MOZ_ASSERT(activation_->isJit());

  if (isWasm()) {
    return wasmIter().stackAddress();
  }

  return jsJitIter().stackAddress();
}

Maybe<JS::ProfilingFrameIterator::Frame>
JS::ProfilingFrameIterator::getPhysicalFrameAndEntry(
    jit::JitcodeGlobalEntry* entry) const {
  void* stackAddr = stackAddress();

  if (isWasm()) {
    Frame frame;
    frame.kind = Frame_Wasm;
    frame.stackAddress = stackAddr;
    frame.returnAddress_ = nullptr;
    frame.activation = activation_;
    frame.label = nullptr;
    frame.endStackAddress = activation_->asJit()->jsOrWasmExitFP();
    frame.interpreterScript = nullptr;
    // TODO: get the realm ID of wasm frames. Bug 1596235.
    frame.realmID = 0;
    return mozilla::Some(frame);
  }

  MOZ_ASSERT(isJSJit());

  // Look up an entry for the return address.
  void* returnAddr = jsJitIter().resumePCinCurrentFrame();
  jit::JitcodeGlobalTable* table =
      cx_->runtime()->jitRuntime()->getJitcodeGlobalTable();

  // NB:
  // The following lookups should be infallible, but the ad-hoc stackwalking
  // code rots easily and corner cases where frames can't be looked up
  // occur too often (e.g. once every day).
  //
  // The calls to `lookup*` below have been changed from infallible ones to
  // fallible ones.  The proper solution to this problem is to fix all
  // the jitcode to use frame-pointers and reliably walk the stack with those.
  const jit::JitcodeGlobalEntry* lookedUpEntry = nullptr;
  if (samplePositionInProfilerBuffer_) {
    lookedUpEntry = table->lookupForSampler(returnAddr, cx_->runtime(),
                                            *samplePositionInProfilerBuffer_);
  } else {
    lookedUpEntry = table->lookup(returnAddr);
  }

  // Failed to look up a jitcode entry for the given address, ignore.
  if (!lookedUpEntry) {
    return mozilla::Nothing();
  }
  *entry = *lookedUpEntry;

  MOZ_ASSERT(entry->isIon() || entry->isBaseline() ||
             entry->isBaselineInterpreter() || entry->isDummy());

  // Dummy frames produce no stack frames.
  if (entry->isDummy()) {
    return mozilla::Nothing();
  }

  Frame frame;
  if (entry->isBaselineInterpreter()) {
    frame.kind = Frame_BaselineInterpreter;
  } else if (entry->isBaseline()) {
    frame.kind = Frame_Baseline;
  } else {
    frame.kind = Frame_Ion;
  }
  frame.stackAddress = stackAddr;
  if (entry->isBaselineInterpreter()) {
    frame.label = jsJitIter().baselineInterpreterLabel();
    jsJitIter().baselineInterpreterScriptPC(
        &frame.interpreterScript, &frame.interpreterPC_, &frame.realmID);
    MOZ_ASSERT(frame.interpreterScript);
    MOZ_ASSERT(frame.interpreterPC_);
  } else {
    frame.interpreterScript = nullptr;
    frame.returnAddress_ = returnAddr;
    frame.label = nullptr;
    frame.realmID = 0;
  }
  frame.activation = activation_;
  frame.endStackAddress = activation_->asJit()->jsOrWasmExitFP();
  return mozilla::Some(frame);
}

uint32_t JS::ProfilingFrameIterator::extractStack(Frame* frames,
                                                  uint32_t offset,
                                                  uint32_t end) const {
  if (offset >= end) {
    return 0;
  }

  jit::JitcodeGlobalEntry entry;
  Maybe<Frame> physicalFrame = getPhysicalFrameAndEntry(&entry);

  // Dummy frames produce no stack frames.
  if (physicalFrame.isNothing()) {
    return 0;
  }

  if (isWasm()) {
    frames[offset] = physicalFrame.value();
    frames[offset].label = wasmIter().label();
    return 1;
  }

  if (physicalFrame->kind == Frame_BaselineInterpreter) {
    frames[offset] = physicalFrame.value();
    return 1;
  }

  // Extract the stack for the entry.  Assume maximum inlining depth is <64
  const char* labels[64];
  uint32_t depth = entry.callStackAtAddr(cx_->runtime(),
                                         jsJitIter().resumePCinCurrentFrame(),
                                         labels, std::size(labels));
  MOZ_ASSERT(depth < std::size(labels));
  for (uint32_t i = 0; i < depth; i++) {
    if (offset + i >= end) {
      return i;
    }
    frames[offset + i] = physicalFrame.value();
    frames[offset + i].label = labels[i];
  }

  return depth;
}

Maybe<JS::ProfilingFrameIterator::Frame>
JS::ProfilingFrameIterator::getPhysicalFrameWithoutLabel() const {
  jit::JitcodeGlobalEntry unused;
  return getPhysicalFrameAndEntry(&unused);
}

bool JS::ProfilingFrameIterator::isWasm() const {
  MOZ_ASSERT(!done());
  return kind_ == Kind::Wasm;
}

bool JS::ProfilingFrameIterator::isJSJit() const {
  return kind_ == Kind::JSJit;
}

mozilla::Maybe<JS::ProfilingFrameIterator::RegisterState>
JS::ProfilingFrameIterator::getCppEntryRegisters() const {
  if (!isJSJit()) {
    return mozilla::Nothing{};
  }
  return jit::JitRuntime::getCppEntryRegisters(jsJitIter().framePtr());
}
