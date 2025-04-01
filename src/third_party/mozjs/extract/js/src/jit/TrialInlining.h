/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TrialInlining_h
#define jit_TrialInlining_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"

#include <stddef.h>
#include <stdint.h>

#include "jstypes.h"
#include "NamespaceImports.h"

#include "gc/Barrier.h"
#include "jit/CacheIR.h"
#include "js/RootingAPI.h"
#include "js/TypeDecls.h"
#include "js/UniquePtr.h"
#include "js/Vector.h"
#include "vm/JSScript.h"

/*
 * [SMDOC] Trial Inlining
 *
 * WarpBuilder relies on transpiling CacheIR. When inlining scripted
 * functions in WarpBuilder, we want our ICs to be as monomorphic as
 * possible. Functions with multiple callers complicate this. An IC in
 * such a function might be monomorphic for any given caller, but
 * polymorphic overall. This make the input to WarpBuilder less precise.
 *
 * To solve this problem, we do trial inlining. During baseline
 * execution, we identify call sites for which it would be useful to
 * have more precise inlining data. For each such call site, we
 * allocate a fresh ICScript and replace the existing call IC with a
 * new specialized IC that invokes the callee using the new
 * ICScript. Other callers of the callee will continue using the
 * default ICScript. When we eventually Warp-compile the script, we
 * can generate code for the callee using the IC information in our
 * private ICScript, which is specialized for its caller.
 *
 * The same approach can be used to inline recursively.
 */

class JS_PUBLIC_API JSTracer;
struct JS_PUBLIC_API JSContext;

class JSFunction;

namespace JS {
class Zone;
}

namespace js {

class BytecodeLocation;

namespace jit {

class BaselineFrame;
class CacheIRWriter;
class ICCacheIRStub;
class ICEntry;
class ICFallbackStub;
class ICScript;
class ICStubSpace;

/*
 * An InliningRoot is owned by a JitScript. In turn, it owns the set
 * of ICScripts that are candidates for being inlined in that JitScript.
 */
class InliningRoot {
 public:
  explicit InliningRoot(JSContext* cx, JSScript* owningScript)
      : owningScript_(owningScript),
        inlinedScripts_(cx),
        totalBytecodeSize_(owningScript->length()) {}

  void trace(JSTracer* trc);
  bool traceWeak(JSTracer* trc);

  bool addInlinedScript(js::UniquePtr<ICScript> icScript);

  uint32_t numInlinedScripts() const { return inlinedScripts_.length(); }

  void purgeInactiveICScripts();

  JSScript* owningScript() const { return owningScript_; }

  size_t totalBytecodeSize() const { return totalBytecodeSize_; }

  void addToTotalBytecodeSize(size_t size) { totalBytecodeSize_ += size; }

  template <typename F>
  void forEachInlinedScript(const F& f) const {
    for (auto& script : inlinedScripts_) {
      f(script.get());
    }
  }

 private:
  HeapPtr<JSScript*> owningScript_;
  js::Vector<js::UniquePtr<ICScript>> inlinedScripts_;

  // Bytecode size of outer script and all inlined scripts.
  size_t totalBytecodeSize_;
};

class InlinableOpData {
 public:
  JSFunction* target = nullptr;
  ICScript* icScript = nullptr;
  const uint8_t* endOfSharedPrefix = nullptr;
};

class InlinableCallData : public InlinableOpData {
 public:
  ObjOperandId calleeOperand;
  CallFlags callFlags;
};

class InlinableGetterData : public InlinableOpData {
 public:
  ValOperandId receiverOperand;
  bool sameRealm = false;
};

class InlinableSetterData : public InlinableOpData {
 public:
  ObjOperandId receiverOperand;
  ValOperandId rhsOperand;
  bool sameRealm = false;
};

mozilla::Maybe<InlinableOpData> FindInlinableOpData(ICCacheIRStub* stub,
                                                    BytecodeLocation loc);

mozilla::Maybe<InlinableCallData> FindInlinableCallData(ICCacheIRStub* stub);
mozilla::Maybe<InlinableGetterData> FindInlinableGetterData(
    ICCacheIRStub* stub);
mozilla::Maybe<InlinableSetterData> FindInlinableSetterData(
    ICCacheIRStub* stub);

enum class TrialInliningDecision {
  NoInline,
  Inline,
  MonomorphicInline,
};

class MOZ_RAII TrialInliner {
 public:
  TrialInliner(JSContext* cx, HandleScript script, ICScript* icScript)
      : cx_(cx), script_(script), icScript_(icScript) {}

  JSContext* cx() { return cx_; }

  [[nodiscard]] bool tryInlining();
  [[nodiscard]] bool maybeInlineCall(ICEntry& entry, ICFallbackStub* fallback,
                                     BytecodeLocation loc);
  [[nodiscard]] bool maybeInlineGetter(ICEntry& entry, ICFallbackStub* fallback,
                                       BytecodeLocation loc, CacheKind kind);
  [[nodiscard]] bool maybeInlineSetter(ICEntry& entry, ICFallbackStub* fallback,
                                       BytecodeLocation loc, CacheKind kind);

  static bool canInline(JSFunction* target, HandleScript caller,
                        BytecodeLocation loc);

  static bool IsValidInliningOp(JSOp op);

 private:
  ICCacheIRStub* maybeSingleStub(const ICEntry& entry);
  void cloneSharedPrefix(ICCacheIRStub* stub, const uint8_t* endOfPrefix,
                         CacheIRWriter& writer);
  ICScript* createInlinedICScript(JSFunction* target, BytecodeLocation loc);
  [[nodiscard]] bool replaceICStub(ICEntry& entry, ICFallbackStub* fallback,
                                   CacheIRWriter& writer, CacheKind kind);

  TrialInliningDecision getInliningDecision(JSFunction* target,
                                            ICCacheIRStub* stub,
                                            BytecodeLocation loc);

  InliningRoot* getOrCreateInliningRoot();
  InliningRoot* maybeGetInliningRoot() const;
  size_t inliningRootTotalBytecodeSize() const;

  JSContext* cx_;
  HandleScript script_;
  ICScript* icScript_;
};

bool DoTrialInlining(JSContext* cx, BaselineFrame* frame);

}  // namespace jit
}  // namespace js

#endif /* jit_TrialInlining_h */
