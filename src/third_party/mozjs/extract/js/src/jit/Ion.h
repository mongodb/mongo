/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Ion_h
#define jit_Ion_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Likely.h"
#include "mozilla/MemoryReporting.h"

#include <stddef.h>
#include <stdint.h>

#include "jsfriendapi.h"
#include "jspubtd.h"

#include "jit/BaselineJIT.h"
#include "jit/IonTypes.h"
#include "jit/JitContext.h"
#include "jit/JitOptions.h"
#include "js/Principals.h"
#include "js/TypeDecls.h"
#include "vm/BytecodeUtil.h"
#include "vm/JSContext.h"
#include "vm/JSFunction.h"
#include "vm/JSScript.h"

namespace js {

class RunState;

namespace jit {

class BaselineFrame;

bool CanIonCompileScript(JSContext* cx, JSScript* script);

[[nodiscard]] bool IonCompileScriptForBaselineAtEntry(JSContext* cx,
                                                      BaselineFrame* frame);

struct IonOsrTempData {
  void* jitcode;
  uint8_t* baselineFrame;

  static constexpr size_t offsetOfJitCode() {
    return offsetof(IonOsrTempData, jitcode);
  }
  static constexpr size_t offsetOfBaselineFrame() {
    return offsetof(IonOsrTempData, baselineFrame);
  }
};

[[nodiscard]] bool IonCompileScriptForBaselineOSR(JSContext* cx,
                                                  BaselineFrame* frame,
                                                  uint32_t frameSize,
                                                  jsbytecode* pc,
                                                  IonOsrTempData** infoPtr);

MethodStatus CanEnterIon(JSContext* cx, RunState& state);

class MIRGenerator;
class LIRGraph;
class CodeGenerator;
class LazyLinkExitFrameLayout;
class WarpSnapshot;

[[nodiscard]] bool OptimizeMIR(MIRGenerator* mir);
LIRGraph* GenerateLIR(MIRGenerator* mir);
CodeGenerator* GenerateCode(MIRGenerator* mir, LIRGraph* lir);
CodeGenerator* CompileBackEnd(MIRGenerator* mir, WarpSnapshot* snapshot);

void LinkIonScript(JSContext* cx, HandleScript calleescript);
uint8_t* LazyLinkTopActivation(JSContext* cx, LazyLinkExitFrameLayout* frame);

inline bool IsIonInlinableGetterOrSetterOp(JSOp op) {
  // JSOp::GetProp, JSOp::CallProp, JSOp::Length, JSOp::GetElem,
  // and JSOp::CallElem. (Inlined Getters)
  // JSOp::SetProp, JSOp::SetName, JSOp::SetGName (Inlined Setters)
  return IsGetPropOp(op) || IsGetElemOp(op) || IsSetPropOp(op);
}

inline bool IsIonInlinableOp(JSOp op) {
  // JSOp::Call, JSOp::FunCall, JSOp::Eval, JSOp::New (Normal Callsites) or an
  // inlinable getter or setter.
  return (IsInvokeOp(op) && !IsSpreadOp(op)) ||
         IsIonInlinableGetterOrSetterOp(op);
}

inline bool TooManyFormalArguments(unsigned nargs) {
  return nargs >= SNAPSHOT_MAX_NARGS || TooManyActualArguments(nargs);
}

inline size_t NumLocalsAndArgs(JSScript* script) {
  size_t num = 1 /* this */ + script->nfixed();
  if (JSFunction* fun = script->function()) {
    num += fun->nargs();
  }
  return num;
}

// Debugging RAII class which marks the current thread as performing an Ion
// backend compilation.
class MOZ_RAII AutoEnterIonBackend {
 public:
  AutoEnterIonBackend() {
#ifdef DEBUG
    JitContext* jcx = GetJitContext();
    jcx->enterIonBackend();
#endif
  }

#ifdef DEBUG
  ~AutoEnterIonBackend() {
    JitContext* jcx = GetJitContext();
    jcx->leaveIonBackend();
  }
#endif
};

bool OffThreadCompilationAvailable(JSContext* cx);

void ForbidCompilation(JSContext* cx, JSScript* script);

size_t SizeOfIonData(JSScript* script, mozilla::MallocSizeOf mallocSizeOf);

inline bool IsIonEnabled(JSContext* cx) {
  if (MOZ_UNLIKELY(!IsBaselineJitEnabled(cx) || cx->options().disableIon())) {
    return false;
  }

  if (MOZ_LIKELY(JitOptions.ion)) {
    return true;
  }
  if (JitOptions.jitForTrustedPrincipals) {
    JS::Realm* realm = js::GetContextRealm(cx);
    return realm && JS::GetRealmPrincipals(realm) &&
           JS::GetRealmPrincipals(realm)->isSystemOrAddonPrincipal();
  }
  return false;
}

// Implemented per-platform.  Returns true if the flags will not require
// further (lazy) computation.
bool CPUFlagsHaveBeenComputed();

}  // namespace jit
}  // namespace js

#endif /* jit_Ion_h */
