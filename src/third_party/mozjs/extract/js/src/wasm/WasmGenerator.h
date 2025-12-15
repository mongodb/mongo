/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_generator_h
#define wasm_generator_h

#include "mozilla/Attributes.h"
#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"

#include "jit/MacroAssembler.h"
#include "jit/PerfSpewer.h"
#include "threading/ProtectedData.h"
#include "vm/HelperThreadTask.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmConstants.h"
#include "wasm/WasmMetadata.h"
#include "wasm/WasmModule.h"

namespace JS {
class OptimizedEncodingListener;
}

namespace js {
namespace wasm {

struct CompileTask;
using CompileTaskPtrVector = Vector<CompileTask*, 0, SystemAllocPolicy>;

// FuncCompileInput contains the input for compiling a single function.

struct FuncCompileInput {
  const uint8_t* begin;
  const uint8_t* end;
  uint32_t index;
  uint32_t lineOrBytecode;
  Uint32Vector callSiteLineNums;

  FuncCompileInput(uint32_t index, uint32_t lineOrBytecode,
                   const uint8_t* begin, const uint8_t* end,
                   Uint32Vector&& callSiteLineNums)
      : begin(begin),
        end(end),
        index(index),
        lineOrBytecode(lineOrBytecode),
        callSiteLineNums(std::move(callSiteLineNums)) {}

  uint32_t bytecodeSize() const {
    static_assert(wasm::MaxFunctionBytes <= UINT32_MAX);
    return uint32_t(end - begin);
  }
};

using FuncCompileInputVector = Vector<FuncCompileInput, 8, SystemAllocPolicy>;

struct FuncCompileOutput {
  FuncCompileOutput(
      uint32_t index, FeatureUsage featureUsage,
      CallRefMetricsRange callRefMetricsRange = CallRefMetricsRange(),
      AllocSitesRange allocSitesRange = AllocSitesRange())
      : index(index),
        featureUsage(featureUsage),
        callRefMetricsRange(callRefMetricsRange),
        allocSitesRange(allocSitesRange) {}

  uint32_t index;
  FeatureUsage featureUsage;
  CallRefMetricsRange callRefMetricsRange;
  AllocSitesRange allocSitesRange;
};

using FuncCompileOutputVector = Vector<FuncCompileOutput, 8, SystemAllocPolicy>;

// CompiledCode contains the resulting code and metadata for a set of compiled
// input functions or stubs.

struct CompiledCode {
  CompiledCode() : featureUsage(FeatureUsage::None) {}

  FuncCompileOutputVector funcs;
  Bytes bytes;
  CodeRangeVector codeRanges;
  InliningContext inliningContext;
  CallSites callSites;
  CallSiteTargetVector callSiteTargets;
  TrapSites trapSites;
  SymbolicAccessVector symbolicAccesses;
  jit::CodeLabelVector codeLabels;
  StackMaps stackMaps;
  TryNoteVector tryNotes;
  CodeRangeUnwindInfoVector codeRangeUnwindInfos;
  CallRefMetricsPatchVector callRefMetricsPatches;
  AllocSitePatchVector allocSitesPatches;
  FuncIonPerfSpewerVector funcIonSpewers;
  FuncBaselinePerfSpewerVector funcBaselineSpewers;
  FeatureUsage featureUsage;
  CompileStats compileStats;

  [[nodiscard]] bool swap(jit::MacroAssembler& masm);

  void clear() {
    funcs.clear();
    bytes.clear();
    codeRanges.clear();
    inliningContext.clear();
    callSites.clear();
    callSiteTargets.clear();
    trapSites.clear();
    symbolicAccesses.clear();
    codeLabels.clear();
    stackMaps.clear();
    tryNotes.clear();
    codeRangeUnwindInfos.clear();
    callRefMetricsPatches.clear();
    allocSitesPatches.clear();
    funcIonSpewers.clear();
    funcBaselineSpewers.clear();
    featureUsage = FeatureUsage::None;
    compileStats.clear();
    MOZ_ASSERT(empty());
  }

  bool empty() {
    return funcs.empty() && bytes.empty() && codeRanges.empty() &&
           inliningContext.empty() && callSites.empty() &&
           callSiteTargets.empty() && trapSites.empty() &&
           symbolicAccesses.empty() && codeLabels.empty() && tryNotes.empty() &&
           stackMaps.empty() && codeRangeUnwindInfos.empty() &&
           callRefMetricsPatches.empty() && allocSitesPatches.empty() &&
           funcIonSpewers.empty() && funcBaselineSpewers.empty() &&
           featureUsage == FeatureUsage::None && compileStats.empty();
  }

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;
};

// The CompileTaskState of a ModuleGenerator contains the mutable state shared
// between helper threads executing CompileTasks. Each CompileTask started on a
// helper thread eventually either ends up in the 'finished' list or increments
// 'numFailed'.

struct CompileTaskState {
  HelperThreadLockData<CompileTaskPtrVector> finished_;
  HelperThreadLockData<uint32_t> numFailed_;
  HelperThreadLockData<UniqueChars> errorMessage_;
  HelperThreadLockData<ConditionVariable> condVar_;

  CompileTaskState() : numFailed_(0) {}
  ~CompileTaskState() {
    MOZ_ASSERT(finished_.refNoCheck().empty());
    MOZ_ASSERT(!numFailed_.refNoCheck());
  }

  CompileTaskPtrVector& finished() { return finished_.ref(); }
  uint32_t& numFailed() { return numFailed_.ref(); }
  UniqueChars& errorMessage() { return errorMessage_.ref(); }
  ConditionVariable& condVar() { return condVar_.ref(); }
};

// A CompileTask holds a batch of input functions that are to be compiled on a
// helper thread as well as, eventually, the results of compilation.

struct CompileTask : public HelperThreadTask {
  const CodeMetadata& codeMeta;
  const CodeTailMetadata* codeTailMeta;
  const CompilerEnvironment& compilerEnv;
  const CompileState compileState;

  CompileTaskState& state;
  LifoAlloc lifo;
  FuncCompileInputVector inputs;
  CompiledCode output;

  CompileTask(const CodeMetadata& codeMeta,
              const CodeTailMetadata* codeTailMeta,
              const CompilerEnvironment& compilerEnv, CompileState compileState,
              CompileTaskState& state, size_t defaultChunkSize)
      : codeMeta(codeMeta),
        codeTailMeta(codeTailMeta),
        compilerEnv(compilerEnv),
        compileState(compileState),
        state(state),
        lifo(defaultChunkSize, js::MallocArena) {}

  virtual ~CompileTask() = default;

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const;

  void runHelperThreadTask(AutoLockHelperThreadState& locked) override;
  ThreadType threadType() override;

  const char* getName() override { return "WasmCompileTask"; }
};

// A ModuleGenerator encapsulates the creation of a wasm module. During the
// lifetime of a ModuleGenerator, a sequence of FunctionGenerators are created
// and destroyed to compile the individual function bodies. After generating all
// functions, ModuleGenerator::finish() must be called to complete the
// compilation and extract the resulting wasm module.

class MOZ_STACK_CLASS ModuleGenerator {
  using CompileTaskVector = Vector<CompileTask, 0, SystemAllocPolicy>;
  using CodeOffsetVector = Vector<jit::CodeOffset, 0, SystemAllocPolicy>;

  // Encapsulates the macro assembler state so that we can create a new one for
  // each code block. Not heap allocated because the macro assembler is a
  // 'stack class'.
  struct MacroAssemblerScope {
    jit::TempAllocator masmAlloc;
    jit::WasmMacroAssembler masm;

    explicit MacroAssemblerScope(LifoAlloc& lifo);
    ~MacroAssemblerScope() = default;
  };

  // Encapsulates all the results of creating a code block.
  struct CodeBlockResult {
    UniqueCodeBlock codeBlock;
    UniqueLinkData linkData;
    FuncIonPerfSpewerVector funcIonSpewers;
    FuncBaselinePerfSpewerVector funcBaselineSpewers;
  };

  // Constant parameters
  SharedCompileArgs const compileArgs_;
  const CompileState compileState_;
  UniqueChars* const error_;
  UniqueCharsVector* const warnings_;
  const mozilla::Atomic<bool>* const cancelled_;
  const CodeMetadata* const codeMeta_;
  const CompilerEnvironment* const compilerEnv_;

  // Data that is used for partial tiering
  SharedCode partialTieringCode_;

  // Data that is used for compiling a complete tier
  mozilla::TimeStamp completeTierStartTime_;

  // Data that is moved into the Module/Code as the result of finish()
  BytecodeRangeVector funcDefRanges_;
  FeatureUsageVector funcDefFeatureUsages_;
  CallRefMetricsRangeVector funcDefCallRefMetrics_;
  AllocSitesRangeVector funcDefAllocSites_;
  FuncImportVector funcImports_;
  CodeBlockResult sharedStubs_;
  MutableCodeMetadataForAsmJS codeMetaForAsmJS_;
  FeatureUsage featureUsage_;

  // Data that is used to construct a CodeBlock
  UniqueCodeBlock codeBlock_;
  UniqueLinkData linkData_;
  LifoAlloc lifo_;
  mozilla::Maybe<MacroAssemblerScope> masmScope_;
  jit::WasmMacroAssembler* masm_;
  uint32_t debugStubCodeOffset_;
  uint32_t requestTierUpStubCodeOffset_;
  uint32_t updateCallRefMetricsStubCodeOffset_;
  CallFarJumpVector callFarJumps_;
  CallSiteTargetVector callSiteTargets_;
  FuncIonPerfSpewerVector funcIonSpewers_;
  FuncBaselinePerfSpewerVector funcBaselineSpewers_;
  uint32_t lastPatchedCallSite_;
  uint32_t startOfUnpatchedCallsites_;
  uint32_t numCallRefMetrics_;
  uint32_t numAllocSites_;
  CompileAndLinkStats tierStats_;

  // Parallel compilation
  bool parallel_;
  uint32_t outstanding_;
  CompileTaskState taskState_;
  CompileTaskVector tasks_;
  CompileTaskPtrVector freeTasks_;
  CompileTask* currentTask_;
  uint32_t batchedBytecode_;

  // Assertions
  mozilla::DebugOnly<bool> finishedFuncDefs_;

  bool funcIsCompiledInBlock(uint32_t funcIndex) const;
  const CodeRange& funcCodeRangeInBlock(uint32_t funcIndex) const;
  bool linkCallSites();
  void noteCodeRange(uint32_t codeRangeIndex, const CodeRange& codeRange);
  bool linkCompiledCode(CompiledCode& code);
  [[nodiscard]] bool initTasks();
  bool locallyCompileCurrentTask();
  bool finishTask(CompileTask* task);
  bool launchBatchCompile();
  bool finishOutstandingTask();

  // Begins the creation of a code block. All code compiled during this time
  // will go into this code block. All previous code blocks must be finished.
  [[nodiscard]] bool startCodeBlock(CodeBlockKind kind);
  // Finish the creation of a code block. This will move all the compiled code
  // and metadata into the code block and initialize it.
  [[nodiscard]] bool finishCodeBlock(CodeBlockResult* result);

  // Generate a code block containing all stubs that are shared between the
  // different tiers.
  [[nodiscard]] bool prepareTier1();

  // Starts the creation of a complete tier of wasm code. Every function
  // defined in this module must be compiled, then finishTier must be
  // called.
  [[nodiscard]] bool startCompleteTier();
  // Starts the creation of a partial tier of wasm code. The specified function
  // must be compiled, then finishTier must be called.
  [[nodiscard]] bool startPartialTier(uint32_t funcIndex);
  // Finishes a complete or partial tier of wasm code.
  [[nodiscard]] bool finishTier(CompileAndLinkStats* tierStats,
                                CodeBlockResult* result);

  bool isAsmJS() const { return codeMeta_->isAsmJS(); }
  Tier tier() const { return compilerEnv_->tier(); }
  CompileMode mode() const { return compilerEnv_->mode(); }
  bool debugEnabled() const { return compilerEnv_->debugEnabled(); }
  bool compilingTier1() const {
    return compileState_ == CompileState::Once ||
           compileState_ == CompileState::EagerTier1 ||
           compileState_ == CompileState::LazyTier1;
  }

  void warnf(const char* msg, ...) MOZ_FORMAT_PRINTF(2, 3);

 public:
  ModuleGenerator(const CodeMetadata& codeMeta,
                  const CompilerEnvironment& compilerEnv,
                  CompileState compilerState,
                  const mozilla::Atomic<bool>* cancelled, UniqueChars* error,
                  UniqueCharsVector* warnings);
  ~ModuleGenerator();
  [[nodiscard]] bool initializeCompleteTier(
      CodeMetadataForAsmJS* codeMetaForAsmJS = nullptr);
  [[nodiscard]] bool initializePartialTier(const Code& code,
                                           uint32_t maybeFuncIndex);

  // Before finishFuncDefs() is called, compileFuncDef() must be called once
  // for each funcIndex in the range [0, env->numFuncDefs()).

  [[nodiscard]] bool compileFuncDef(
      uint32_t funcIndex, uint32_t lineOrBytecode, const uint8_t* begin,
      const uint8_t* end, Uint32Vector&& callSiteLineNums = Uint32Vector());

  // Must be called after the last compileFuncDef() and before finishModule()
  // or finishTier2().

  [[nodiscard]] bool finishFuncDefs();

  // If env->mode is Once or Tier1, finishModule() must be called to generate
  // a new Module. Otherwise, if env->mode is Tier2, finishTier2() must be
  // called to augment the given Module with tier 2 code.  `moduleMeta`
  // is passed as mutable only because we have to std::move field(s) out of
  // it; if that in future gets cleaned up, the parameter should be changed
  // to being SharedModuleMetadata.

  SharedModule finishModule(
      const BytecodeBufferOrSource& bytecode, ModuleMetadata& moduleMeta,
      JS::OptimizedEncodingListener* maybeCompleteTier2Listener);
  [[nodiscard]] bool finishTier2(const Module& module);
  [[nodiscard]] bool finishPartialTier2();
};

}  // namespace wasm
}  // namespace js

#endif  // wasm_generator_h
