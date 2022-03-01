/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_TrialInlining_h
#define jit_TrialInlining_h

#include "jit/CacheIR.h"
#include "jit/ICStubSpace.h"
#include "vm/BytecodeLocation.h"

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

namespace js {
namespace jit {

class BaselineFrame;
class ICEntry;
class ICScript;
class ICFallbackStub;

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

  JitScriptICStubSpace* jitScriptStubSpace() { return &jitScriptStubSpace_; }

  void trace(JSTracer* trc);

  bool addInlinedScript(js::UniquePtr<ICScript> icScript);

  uint32_t numInlinedScripts() const { return inlinedScripts_.length(); }

  void purgeOptimizedStubs(Zone* zone);
  void resetWarmUpCounts(uint32_t count);

  JSScript* owningScript() const { return owningScript_; }

  size_t totalBytecodeSize() const { return totalBytecodeSize_; }

  void addToTotalBytecodeSize(size_t size) { totalBytecodeSize_ += size; }

 private:
  JitScriptICStubSpace jitScriptStubSpace_ = {};
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

class MOZ_RAII TrialInliner {
 public:
  TrialInliner(JSContext* cx, HandleScript script, ICScript* icScript,
               InliningRoot* root)
      : cx_(cx), script_(script), icScript_(icScript), root_(root) {}

  JSContext* cx() { return cx_; }

  [[nodiscard]] bool tryInlining();
  [[nodiscard]] bool maybeInlineCall(ICEntry& entry, ICFallbackStub* fallback,
                                     BytecodeLocation loc);
  [[nodiscard]] bool maybeInlineGetter(ICEntry& entry, ICFallbackStub* fallback,
                                       BytecodeLocation loc);
  [[nodiscard]] bool maybeInlineSetter(ICEntry& entry, ICFallbackStub* fallback,
                                       BytecodeLocation loc);

  static bool canInline(JSFunction* target, HandleScript caller,
                        BytecodeLocation loc);

 private:
  ICCacheIRStub* maybeSingleStub(const ICEntry& entry);
  void cloneSharedPrefix(ICCacheIRStub* stub, const uint8_t* endOfPrefix,
                         CacheIRWriter& writer);
  ICScript* createInlinedICScript(JSFunction* target, BytecodeLocation loc);
  [[nodiscard]] bool replaceICStub(ICEntry& entry, ICFallbackStub* fallback,
                                   CacheIRWriter& writer, CacheKind kind);

  bool shouldInline(JSFunction* target, ICCacheIRStub* stub,
                    BytecodeLocation loc);

  JSContext* cx_;
  HandleScript script_;
  ICScript* icScript_;
  InliningRoot* root_;
};

bool DoTrialInlining(JSContext* cx, BaselineFrame* frame);

}  // namespace jit
}  // namespace js

#endif /* jit_TrialInlining_h */
