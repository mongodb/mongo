/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/Jit.h"

#include "jit/BaselineJIT.h"
#include "jit/CalleeToken.h"
#include "jit/Ion.h"
#include "jit/JitCommon.h"
#include "jit/JitRuntime.h"
#include "js/friend/StackLimits.h"  // js::AutoCheckRecursionLimit
#include "vm/Interpreter.h"
#include "vm/JitActivation.h"
#include "vm/JSContext.h"
#include "vm/PortableBaselineInterpret.h"
#include "vm/Realm.h"

#include "vm/Activation-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

static EnterJitStatus JS_HAZ_JSNATIVE_CALLER EnterJit(JSContext* cx,
                                                      RunState& state,
                                                      uint8_t* code) {
  // We don't want to call the interpreter stub here (because
  // C++ -> interpreterStub -> C++ is slower than staying in C++).
  MOZ_ASSERT(code);
#ifndef ENABLE_PORTABLE_BASELINE_INTERP
  MOZ_ASSERT(code != cx->runtime()->jitRuntime()->interpreterStub().value);
  MOZ_ASSERT(IsBaselineInterpreterEnabled());
#else
  MOZ_ASSERT(IsBaselineInterpreterEnabled() ||
             IsPortableBaselineInterpreterEnabled());
#endif

  AutoCheckRecursionLimit recursion(cx);
  if (!recursion.check(cx)) {
    return EnterJitStatus::Error;
  }

  // jit::Bailout(), jit::InvalidationBailout(), and jit::HandleException()
  // reset the counter to zero, so assert here it's also zero when we enter
  // JIT code.
  MOZ_ASSERT(!cx->isInUnsafeRegion());

#ifdef DEBUG
  // Assert we don't GC before entering JIT code. A GC could discard JIT code
  // or move the function stored in the CalleeToken (it won't be traced at
  // this point). We use Maybe<> here so we can call reset() to call the
  // AutoAssertNoGC destructor before we enter JIT code.
  mozilla::Maybe<JS::AutoAssertNoGC> nogc;
  nogc.emplace(cx);
#endif

  JSScript* script = state.script();
  size_t numActualArgs;
  bool constructing;
  size_t maxArgc;
  Value* maxArgv;
  JSObject* envChain;
  CalleeToken calleeToken;

  unsigned numFormals = 0;
  if (state.isInvoke()) {
    const CallArgs& args = state.asInvoke()->args();
    numActualArgs = args.length();

    if (TooManyActualArguments(numActualArgs)) {
      // Fall back to the C++ interpreter to avoid running out of stack space.
      return EnterJitStatus::NotEntered;
    }

    constructing = state.asInvoke()->constructing();
    maxArgc = args.length() + 1;
    maxArgv = args.array() - 1;  // -1 to include |this|
    envChain = nullptr;
    calleeToken = CalleeToToken(&args.callee().as<JSFunction>(), constructing);

    numFormals = script->function()->nargs();
    if (numFormals > numActualArgs) {
#ifndef ENABLE_PORTABLE_BASELINE_INTERP
      code = cx->runtime()->jitRuntime()->getArgumentsRectifier().value;
#endif
    }
  } else {
    numActualArgs = 0;
    constructing = false;
    maxArgc = 0;
    maxArgv = nullptr;
    envChain = state.asExecute()->environmentChain();
    calleeToken = CalleeToToken(state.script());
  }

  // Caller must construct |this| before invoking the function.
  MOZ_ASSERT_IF(constructing, maxArgv[0].isObject() ||
                                  maxArgv[0].isMagic(JS_UNINITIALIZED_LEXICAL));

  RootedValue result(cx, Int32Value(numActualArgs));
  {
    AssertRealmUnchanged aru(cx);
    ActivationEntryMonitor entryMonitor(cx, calleeToken);
    JitActivation activation(cx);

#ifndef ENABLE_PORTABLE_BASELINE_INTERP
    EnterJitCode enter = cx->runtime()->jitRuntime()->enterJit();

#  ifdef DEBUG
    nogc.reset();
#  endif
    CALL_GENERATED_CODE(enter, code, maxArgc, maxArgv, /* osrFrame = */ nullptr,
                        calleeToken, envChain, /* osrNumStackValues = */ 0,
                        result.address());
#else  // !ENABLE_PORTABLE_BASELINE_INTERP
    (void)code;
#  ifdef DEBUG
    nogc.reset();
#  endif
    if (!pbl::PortablebaselineInterpreterStackCheck(cx, state, numActualArgs)) {
      return EnterJitStatus::NotEntered;
    }
    if (!pbl::PortableBaselineTrampoline(cx, maxArgc, maxArgv, numFormals,
                                         numActualArgs, calleeToken, envChain,
                                         result.address())) {
      return EnterJitStatus::Error;
    }
#endif  // ENABLE_PORTABLE_BASELINE_INTERP
  }

  // Ensure the counter was reset to zero after exiting from JIT code.
  MOZ_ASSERT(!cx->isInUnsafeRegion());

  // Release temporary buffer used for OSR into Ion.
  if (!IsPortableBaselineInterpreterEnabled()) {
    cx->runtime()->jitRuntime()->freeIonOsrTempData();
  }

  if (result.isMagic()) {
    MOZ_ASSERT(result.isMagic(JS_ION_ERROR));
    return EnterJitStatus::Error;
  }

  // Jit callers wrap primitive constructor return, except for derived
  // class constructors, which are forced to do it themselves.
  if (constructing && result.isPrimitive()) {
    MOZ_ASSERT(maxArgv[0].isObject());
    result = maxArgv[0];
  }

  state.setReturnValue(result);
  return EnterJitStatus::Ok;
}

// Call the per-script interpreter entry trampoline.
bool js::jit::EnterInterpreterEntryTrampoline(uint8_t* code, JSContext* cx,
                                              RunState* state) {
  using EnterTrampolineCodePtr = bool (*)(JSContext* cx, RunState*);
  auto funcPtr = JS_DATA_TO_FUNC_PTR(EnterTrampolineCodePtr, code);
  return CALL_GENERATED_2(funcPtr, cx, state);
}

EnterJitStatus js::jit::MaybeEnterJit(JSContext* cx, RunState& state) {
  if (!IsBaselineInterpreterEnabled()
#ifdef ENABLE_PORTABLE_BASELINE_INTERP
      && !IsPortableBaselineInterpreterEnabled()
#endif
  ) {
    // All JITs are disabled.
    return EnterJitStatus::NotEntered;
  }

  // JITs do not respect the debugger's OnNativeCall hook, so JIT execution is
  // disabled if this hook might need to be called.
  if (cx->realm()->debuggerObservesNativeCall()) {
    return EnterJitStatus::NotEntered;
  }

  JSScript* script = state.script();

  uint8_t* code = script->jitCodeRaw();

#ifdef JS_CACHEIR_SPEW
  cx->spewer().enableSpewing();
#endif

  do {
    // Make sure we can enter Baseline Interpreter code. Note that the prologue
    // has warm-up checks to tier up if needed.
    if (script->hasJitScript() && code) {
      break;
    }

    script->incWarmUpCounter();

#ifndef ENABLE_PORTABLE_BASELINE_INTERP
    // Try to Ion-compile.
    if (jit::IsIonEnabled(cx)) {
      jit::MethodStatus status = jit::CanEnterIon(cx, state);
      if (status == jit::Method_Error) {
        return EnterJitStatus::Error;
      }
      if (status == jit::Method_Compiled) {
        code = script->jitCodeRaw();
        break;
      }
    }

    // Try to Baseline-compile.
    if (jit::IsBaselineJitEnabled(cx)) {
      jit::MethodStatus status =
          jit::CanEnterBaselineMethod<BaselineTier::Compiler>(cx, state);
      if (status == jit::Method_Error) {
        return EnterJitStatus::Error;
      }
      if (status == jit::Method_Compiled) {
        code = script->jitCodeRaw();
        break;
      }
    }

    // Try to enter the Baseline Interpreter.
    if (IsBaselineInterpreterEnabled()) {
      jit::MethodStatus status =
          jit::CanEnterBaselineMethod<BaselineTier::Interpreter>(cx, state);
      if (status == jit::Method_Error) {
        return EnterJitStatus::Error;
      }
      if (status == jit::Method_Compiled) {
        code = script->jitCodeRaw();
        break;
      }
    }

#else  // !ENABLE_PORTABLE_BASELINE_INTERP

    // Try to enter the Portable Baseline Interpreter.
    if (IsPortableBaselineInterpreterEnabled()) {
      jit::MethodStatus status =
          pbl::CanEnterPortableBaselineInterpreter(cx, state);
      if (status == jit::Method_Error) {
        return EnterJitStatus::Error;
      }
      if (status == jit::Method_Compiled) {
        code = script->jitCodeRaw();
        break;
      }
    }
#endif

    return EnterJitStatus::NotEntered;
  } while (false);

#ifdef JS_CACHEIR_SPEW
  cx->spewer().disableSpewing();
#endif

  return EnterJit(cx, state, code);
}
