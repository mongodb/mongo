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

#include "wasm/WasmGenerator.h"

#include "mozilla/CheckedInt.h"
#include "mozilla/EnumeratedRange.h"
#include "mozilla/SHA1.h"

#include <algorithm>

#include "jit/Assembler.h"
#include "jit/JitOptions.h"
#include "js/Printf.h"
#include "threading/Thread.h"
#include "util/Memory.h"
#include "util/Text.h"
#include "vm/HelperThreads.h"
#include "vm/Time.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmCompile.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmSummarizeInsn.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::CheckedInt;
using mozilla::MakeEnumeratedRange;

bool CompiledCode::swap(MacroAssembler& masm) {
  MOZ_ASSERT(bytes.empty());
  if (!masm.swapBuffer(bytes)) {
    return false;
  }

  callSites.swap(masm.callSites());
  callSiteTargets.swap(masm.callSiteTargets());
  trapSites.swap(masm.trapSites());
  symbolicAccesses.swap(masm.symbolicAccesses());
  tryNotes.swap(masm.tryNotes());
  codeRangeUnwindInfos.swap(masm.codeRangeUnwindInfos());
  codeLabels.swap(masm.codeLabels());
  return true;
}

// ****************************************************************************
// ModuleGenerator

static const unsigned GENERATOR_LIFO_DEFAULT_CHUNK_SIZE = 4 * 1024;
static const unsigned COMPILATION_LIFO_DEFAULT_CHUNK_SIZE = 64 * 1024;
static const uint32_t BAD_CODE_RANGE = UINT32_MAX;

ModuleGenerator::ModuleGenerator(const CompileArgs& args,
                                 ModuleEnvironment* moduleEnv,
                                 CompilerEnvironment* compilerEnv,
                                 const Atomic<bool>* cancelled,
                                 UniqueChars* error,
                                 UniqueCharsVector* warnings)
    : compileArgs_(&args),
      error_(error),
      warnings_(warnings),
      cancelled_(cancelled),
      moduleEnv_(moduleEnv),
      compilerEnv_(compilerEnv),
      linkData_(nullptr),
      metadataTier_(nullptr),
      lifo_(GENERATOR_LIFO_DEFAULT_CHUNK_SIZE),
      masmAlloc_(&lifo_),
      masm_(masmAlloc_, *moduleEnv, /* limitedSize= */ false),
      debugTrapCodeOffset_(),
      lastPatchedCallSite_(0),
      startOfUnpatchedCallsites_(0),
      parallel_(false),
      outstanding_(0),
      currentTask_(nullptr),
      batchedBytecode_(0),
      finishedFuncDefs_(false) {}

ModuleGenerator::~ModuleGenerator() {
  MOZ_ASSERT_IF(finishedFuncDefs_, !batchedBytecode_);
  MOZ_ASSERT_IF(finishedFuncDefs_, !currentTask_);

  if (parallel_) {
    if (outstanding_) {
      AutoLockHelperThreadState lock;

      // Remove any pending compilation tasks from the worklist.
      size_t removed = RemovePendingWasmCompileTasks(taskState_, mode(), lock);
      MOZ_ASSERT(outstanding_ >= removed);
      outstanding_ -= removed;

      // Wait until all active compilation tasks have finished.
      while (true) {
        MOZ_ASSERT(outstanding_ >= taskState_.finished().length());
        outstanding_ -= taskState_.finished().length();
        taskState_.finished().clear();

        MOZ_ASSERT(outstanding_ >= taskState_.numFailed());
        outstanding_ -= taskState_.numFailed();
        taskState_.numFailed() = 0;

        if (!outstanding_) {
          break;
        }

        taskState_.condVar().wait(lock); /* failed or finished */
      }
    }
  } else {
    MOZ_ASSERT(!outstanding_);
  }

  // Propagate error state.
  if (error_ && !*error_) {
    AutoLockHelperThreadState lock;
    *error_ = std::move(taskState_.errorMessage());
  }
}

// This is the highest offset into Instance::globalArea that will not overflow
// a signed 32-bit integer.
static const uint32_t MaxInstanceDataOffset =
    INT32_MAX - Instance::offsetOfData();

bool ModuleGenerator::allocateInstanceDataBytes(uint32_t bytes, uint32_t align,
                                                uint32_t* instanceDataOffset) {
  CheckedInt<uint32_t> newInstanceDataLength(metadata_->instanceDataLength);

  // Adjust the current global data length so that it's aligned to `align`
  newInstanceDataLength +=
      ComputeByteAlignment(newInstanceDataLength.value(), align);
  if (!newInstanceDataLength.isValid()) {
    return false;
  }

  // The allocated data is given by the aligned length
  *instanceDataOffset = newInstanceDataLength.value();

  // Advance the length for `bytes` being allocated
  newInstanceDataLength += bytes;
  if (!newInstanceDataLength.isValid()) {
    return false;
  }

  // Check that the highest offset into this allocated space would not overflow
  // a signed 32-bit integer.
  if (newInstanceDataLength.value() > MaxInstanceDataOffset + 1) {
    return false;
  }

  metadata_->instanceDataLength = newInstanceDataLength.value();
  return true;
}

bool ModuleGenerator::allocateInstanceDataBytesN(uint32_t bytes, uint32_t align,
                                                 uint32_t count,
                                                 uint32_t* instanceDataOffset) {
  // The size of each allocation should be a multiple of alignment so that a
  // contiguous array of allocations will be aligned
  MOZ_ASSERT(bytes % align == 0);

  // Compute the total bytes being allocated
  CheckedInt<uint32_t> totalBytes = bytes;
  totalBytes *= count;
  if (!totalBytes.isValid()) {
    return false;
  }

  // Allocate the bytes
  return allocateInstanceDataBytes(totalBytes.value(), align,
                                   instanceDataOffset);
}

bool ModuleGenerator::init(Metadata* maybeAsmJSMetadata) {
  // Perform fallible metadata, linkdata, assumption allocations.

  MOZ_ASSERT(isAsmJS() == !!maybeAsmJSMetadata);
  if (maybeAsmJSMetadata) {
    metadata_ = maybeAsmJSMetadata;
  } else {
    metadata_ = js_new<Metadata>();
    if (!metadata_) {
      return false;
    }
  }

  if (compileArgs_->scriptedCaller.filename) {
    metadata_->filename =
        DuplicateString(compileArgs_->scriptedCaller.filename.get());
    if (!metadata_->filename) {
      return false;
    }

    metadata_->filenameIsURL = compileArgs_->scriptedCaller.filenameIsURL;
  } else {
    MOZ_ASSERT(!compileArgs_->scriptedCaller.filenameIsURL);
  }

  if (compileArgs_->sourceMapURL) {
    metadata_->sourceMapURL = DuplicateString(compileArgs_->sourceMapURL.get());
    if (!metadata_->sourceMapURL) {
      return false;
    }
  }

  linkData_ = js::MakeUnique<LinkData>(tier());
  if (!linkData_) {
    return false;
  }

  metadataTier_ = js::MakeUnique<MetadataTier>(tier());
  if (!metadataTier_) {
    return false;
  }

  // funcToCodeRange maps function indices to code-range indices and all
  // elements will be initialized by the time module generation is finished.

  if (!metadataTier_->funcToCodeRange.appendN(BAD_CODE_RANGE,
                                              moduleEnv_->funcs.length())) {
    return false;
  }

  // Pre-reserve space for large Vectors to avoid the significant cost of the
  // final reallocs. In particular, the MacroAssembler can be enormous, so be
  // extra conservative. Since large over-reservations may fail when the
  // actual allocations will succeed, ignore OOM failures. Note,
  // shrinkStorageToFit calls at the end will trim off unneeded capacity.

  size_t codeSectionSize =
      moduleEnv_->codeSection ? moduleEnv_->codeSection->size : 0;

  size_t estimatedCodeSize =
      size_t(1.2 * EstimateCompiledCodeSize(tier(), codeSectionSize));
  (void)masm_.reserve(std::min(estimatedCodeSize, MaxCodeBytesPerProcess));

  (void)metadataTier_->codeRanges.reserve(2 * moduleEnv_->numFuncDefs());

  const size_t ByteCodesPerCallSite = 50;
  (void)metadataTier_->callSites.reserve(codeSectionSize /
                                         ByteCodesPerCallSite);

  const size_t ByteCodesPerOOBTrap = 10;
  (void)metadataTier_->trapSites[Trap::OutOfBounds].reserve(
      codeSectionSize / ByteCodesPerOOBTrap);

  // Allocate space in instance for declarations that need it
  MOZ_ASSERT(metadata_->instanceDataLength == 0);

  // Allocate space for type definitions
  if (!allocateInstanceDataBytesN(
          sizeof(TypeDefInstanceData), alignof(TypeDefInstanceData),
          moduleEnv_->types->length(), &moduleEnv_->typeDefsOffsetStart)) {
    return false;
  }
  metadata_->typeDefsOffsetStart = moduleEnv_->typeDefsOffsetStart;

  // Allocate space for every function import
  if (!allocateInstanceDataBytesN(
          sizeof(FuncImportInstanceData), alignof(FuncImportInstanceData),
          moduleEnv_->numFuncImports, &moduleEnv_->funcImportsOffsetStart)) {
    return false;
  }

  // Allocate space for every memory
  if (!allocateInstanceDataBytesN(
          sizeof(MemoryInstanceData), alignof(MemoryInstanceData),
          moduleEnv_->memories.length(), &moduleEnv_->memoriesOffsetStart)) {
    return false;
  }
  metadata_->memoriesOffsetStart = moduleEnv_->memoriesOffsetStart;

  // Allocate space for every table
  if (!allocateInstanceDataBytesN(
          sizeof(TableInstanceData), alignof(TableInstanceData),
          moduleEnv_->tables.length(), &moduleEnv_->tablesOffsetStart)) {
    return false;
  }
  metadata_->tablesOffsetStart = moduleEnv_->tablesOffsetStart;

  // Allocate space for every tag
  if (!allocateInstanceDataBytesN(
          sizeof(TagInstanceData), alignof(TagInstanceData),
          moduleEnv_->tags.length(), &moduleEnv_->tagsOffsetStart)) {
    return false;
  }
  metadata_->tagsOffsetStart = moduleEnv_->tagsOffsetStart;

  // Allocate space for every global that requires it
  for (GlobalDesc& global : moduleEnv_->globals) {
    if (global.isConstant()) {
      continue;
    }

    uint32_t width = global.isIndirect() ? sizeof(void*) : global.type().size();

    uint32_t instanceDataOffset;
    if (!allocateInstanceDataBytes(width, width, &instanceDataOffset)) {
      return false;
    }

    global.setOffset(instanceDataOffset);
  }

  // Initialize function import metadata
  if (!metadataTier_->funcImports.resize(moduleEnv_->numFuncImports)) {
    return false;
  }

  for (size_t i = 0; i < moduleEnv_->numFuncImports; i++) {
    metadataTier_->funcImports[i] =
        FuncImport(moduleEnv_->funcs[i].typeIndex,
                   moduleEnv_->offsetOfFuncImportInstanceData(i));
  }

  // Share type definitions with metadata
  metadata_->types = moduleEnv_->types;

  // Accumulate all exported functions:
  // - explicitly marked as such;
  // - implicitly exported by being an element of function tables;
  // - implicitly exported by being the start function;
  // - implicitly exported by being used in global ref.func initializer
  // ModuleEnvironment accumulates this information for us during decoding,
  // transfer it to the FuncExportVector stored in Metadata.

  uint32_t exportedFuncCount = 0;
  for (const FuncDesc& func : moduleEnv_->funcs) {
    if (func.isExported()) {
      exportedFuncCount++;
    }
  }
  if (!metadataTier_->funcExports.reserve(exportedFuncCount)) {
    return false;
  }

  for (uint32_t funcIndex = 0; funcIndex < moduleEnv_->funcs.length();
       funcIndex++) {
    const FuncDesc& func = moduleEnv_->funcs[funcIndex];

    if (!func.isExported()) {
      continue;
    }

    metadataTier_->funcExports.infallibleEmplaceBack(
        FuncExport(func.typeIndex, funcIndex, func.isEager()));
  }

  // Determine whether parallel or sequential compilation is to be used and
  // initialize the CompileTasks that will be used in either mode.

  MOZ_ASSERT(GetHelperThreadCount() > 1);

  uint32_t numTasks;
  if (CanUseExtraThreads() && GetHelperThreadCPUCount() > 1) {
    parallel_ = true;
    numTasks = 2 * GetMaxWasmCompilationThreads();
  } else {
    numTasks = 1;
  }

  if (!tasks_.initCapacity(numTasks)) {
    return false;
  }
  for (size_t i = 0; i < numTasks; i++) {
    tasks_.infallibleEmplaceBack(*moduleEnv_, *compilerEnv_, taskState_,
                                 COMPILATION_LIFO_DEFAULT_CHUNK_SIZE);
  }

  if (!freeTasks_.reserve(numTasks)) {
    return false;
  }
  for (size_t i = 0; i < numTasks; i++) {
    freeTasks_.infallibleAppend(&tasks_[i]);
  }

  // Fill in function stubs for each import so that imported functions can be
  // used in all the places that normal function definitions can (table
  // elements, export calls, etc).

  CompiledCode& importCode = tasks_[0].output;
  MOZ_ASSERT(importCode.empty());

  if (!GenerateImportFunctions(*moduleEnv_, metadataTier_->funcImports,
                               &importCode)) {
    return false;
  }

  if (!linkCompiledCode(importCode)) {
    return false;
  }

  importCode.clear();
  return true;
}

bool ModuleGenerator::funcIsCompiled(uint32_t funcIndex) const {
  return metadataTier_->funcToCodeRange[funcIndex] != BAD_CODE_RANGE;
}

const CodeRange& ModuleGenerator::funcCodeRange(uint32_t funcIndex) const {
  MOZ_ASSERT(funcIsCompiled(funcIndex));
  const CodeRange& cr =
      metadataTier_->codeRanges[metadataTier_->funcToCodeRange[funcIndex]];
  MOZ_ASSERT(cr.isFunction());
  return cr;
}

static bool InRange(uint32_t caller, uint32_t callee) {
  // We assume JumpImmediateRange is defined conservatively enough that the
  // slight difference between 'caller' (which is really the return address
  // offset) and the actual base of the relative displacement computation
  // isn't significant.
  uint32_t range = std::min(JitOptions.jumpThreshold, JumpImmediateRange);
  if (caller < callee) {
    return callee - caller < range;
  }
  return caller - callee < range;
}

using OffsetMap =
    HashMap<uint32_t, uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy>;
using TrapMaybeOffsetArray =
    EnumeratedArray<Trap, Maybe<uint32_t>, size_t(Trap::Limit)>;

bool ModuleGenerator::linkCallSites() {
  AutoCreatedBy acb(masm_, "linkCallSites");

  masm_.haltingAlign(CodeAlignment);

  // Create far jumps for calls that have relative offsets that may otherwise
  // go out of range. This method is called both between function bodies (at a
  // frequency determined by the ISA's jump range) and once at the very end of
  // a module's codegen after all possible calls/traps have been emitted.

  OffsetMap existingCallFarJumps;
  for (; lastPatchedCallSite_ < metadataTier_->callSites.length();
       lastPatchedCallSite_++) {
    const CallSite& callSite = metadataTier_->callSites[lastPatchedCallSite_];
    const CallSiteTarget& target = callSiteTargets_[lastPatchedCallSite_];
    uint32_t callerOffset = callSite.returnAddressOffset();
    switch (callSite.kind()) {
      case CallSiteDesc::Import:
      case CallSiteDesc::Indirect:
      case CallSiteDesc::IndirectFast:
      case CallSiteDesc::Symbolic:
      case CallSiteDesc::Breakpoint:
      case CallSiteDesc::EnterFrame:
      case CallSiteDesc::LeaveFrame:
      case CallSiteDesc::CollapseFrame:
      case CallSiteDesc::FuncRef:
      case CallSiteDesc::FuncRefFast:
      case CallSiteDesc::ReturnStub:
      case CallSiteDesc::StackSwitch:
        break;
      case CallSiteDesc::ReturnFunc:
      case CallSiteDesc::Func: {
        auto patch = [this, callSite](uint32_t callerOffset,
                                      uint32_t calleeOffset) {
          if (callSite.kind() == CallSiteDesc::ReturnFunc) {
            masm_.patchFarJump(CodeOffset(callerOffset), calleeOffset);
          } else {
            MOZ_ASSERT(callSite.kind() == CallSiteDesc::Func);
            masm_.patchCall(callerOffset, calleeOffset);
          }
        };
        if (funcIsCompiled(target.funcIndex())) {
          uint32_t calleeOffset =
              funcCodeRange(target.funcIndex()).funcUncheckedCallEntry();
          if (InRange(callerOffset, calleeOffset)) {
            patch(callerOffset, calleeOffset);
            break;
          }
        }

        OffsetMap::AddPtr p =
            existingCallFarJumps.lookupForAdd(target.funcIndex());
        if (!p) {
          Offsets offsets;
          offsets.begin = masm_.currentOffset();
          if (!callFarJumps_.emplaceBack(target.funcIndex(),
                                         masm_.farJumpWithPatch())) {
            return false;
          }
          offsets.end = masm_.currentOffset();
          if (masm_.oom()) {
            return false;
          }
          if (!metadataTier_->codeRanges.emplaceBack(CodeRange::FarJumpIsland,
                                                     offsets)) {
            return false;
          }
          if (!existingCallFarJumps.add(p, target.funcIndex(), offsets.begin)) {
            return false;
          }
        }

        patch(callerOffset, p->value());
        break;
      }
    }
  }

  masm_.flushBuffer();
  return !masm_.oom();
}

void ModuleGenerator::noteCodeRange(uint32_t codeRangeIndex,
                                    const CodeRange& codeRange) {
  switch (codeRange.kind()) {
    case CodeRange::Function:
      MOZ_ASSERT(metadataTier_->funcToCodeRange[codeRange.funcIndex()] ==
                 BAD_CODE_RANGE);
      metadataTier_->funcToCodeRange[codeRange.funcIndex()] = codeRangeIndex;
      break;
    case CodeRange::InterpEntry:
      metadataTier_->lookupFuncExport(codeRange.funcIndex())
          .initEagerInterpEntryOffset(codeRange.begin());
      break;
    case CodeRange::JitEntry:
      // Nothing to do: jit entries are linked in the jump tables.
      break;
    case CodeRange::ImportJitExit:
      metadataTier_->funcImports[codeRange.funcIndex()].initJitExitOffset(
          codeRange.begin());
      break;
    case CodeRange::ImportInterpExit:
      metadataTier_->funcImports[codeRange.funcIndex()].initInterpExitOffset(
          codeRange.begin());
      break;
    case CodeRange::DebugTrap:
      MOZ_ASSERT(!debugTrapCodeOffset_);
      debugTrapCodeOffset_ = codeRange.begin();
      break;
    case CodeRange::TrapExit:
      MOZ_ASSERT(!linkData_->trapOffset);
      linkData_->trapOffset = codeRange.begin();
      break;
    case CodeRange::Throw:
      // Jumped to by other stubs, so nothing to do.
      break;
    case CodeRange::FarJumpIsland:
    case CodeRange::BuiltinThunk:
      MOZ_CRASH("Unexpected CodeRange kind");
  }
}

// Append every element from `srcVec` where `filterOp(srcElem) == true`.
// Applies `mutateOp(dstElem)` to every element that is appended.
template <class Vec, class FilterOp, class MutateOp>
static bool AppendForEach(Vec* dstVec, const Vec& srcVec, FilterOp filterOp,
                          MutateOp mutateOp) {
  // Eagerly grow the vector to the whole src vector. Any filtered elements
  // will be trimmed later.
  if (!dstVec->growByUninitialized(srcVec.length())) {
    return false;
  }

  using T = typename Vec::ElementType;

  T* dstBegin = dstVec->begin();
  T* dstEnd = dstVec->end();

  // We appended srcVec.length() elements at the beginning, so we append
  // elements starting at the first uninitialized element.
  T* dst = dstEnd - srcVec.length();

  for (const T* src = srcVec.begin(); src != srcVec.end(); src++) {
    if (!filterOp(src)) {
      continue;
    }
    new (dst) T(*src);
    mutateOp(dst - dstBegin, dst);
    dst++;
  }

  // Trim off the filtered out elements that were eagerly added at the
  // beginning
  size_t newSize = dst - dstBegin;
  if (newSize != dstVec->length()) {
    dstVec->shrinkTo(newSize);
  }

  return true;
}

template <typename T>
bool FilterNothing(const T* element) {
  return true;
}

// The same as the above `AppendForEach`, without performing any filtering.
template <class Vec, class MutateOp>
static bool AppendForEach(Vec* dstVec, const Vec& srcVec, MutateOp mutateOp) {
  using T = typename Vec::ElementType;
  return AppendForEach(dstVec, srcVec, &FilterNothing<T>, mutateOp);
}

bool ModuleGenerator::linkCompiledCode(CompiledCode& code) {
  AutoCreatedBy acb(masm_, "ModuleGenerator::linkCompiledCode");
  JitContext jcx;

  // Combine observed features from the compiled code into the metadata
  metadata_->featureUsage |= code.featureUsage;

  // Before merging in new code, if calls in a prior code range might go out of
  // range, insert far jumps to extend the range.

  if (!InRange(startOfUnpatchedCallsites_,
               masm_.size() + code.bytes.length())) {
    startOfUnpatchedCallsites_ = masm_.size();
    if (!linkCallSites()) {
      return false;
    }
  }

  // All code offsets in 'code' must be incremented by their position in the
  // overall module when the code was appended.

  masm_.haltingAlign(CodeAlignment);
  const size_t offsetInModule = masm_.size();
  if (!masm_.appendRawCode(code.bytes.begin(), code.bytes.length())) {
    return false;
  }

  auto codeRangeOp = [offsetInModule, this](uint32_t codeRangeIndex,
                                            CodeRange* codeRange) {
    codeRange->offsetBy(offsetInModule);
    noteCodeRange(codeRangeIndex, *codeRange);
  };
  if (!AppendForEach(&metadataTier_->codeRanges, code.codeRanges,
                     codeRangeOp)) {
    return false;
  }

  auto callSiteOp = [=](uint32_t, CallSite* cs) {
    cs->offsetBy(offsetInModule);
  };
  if (!AppendForEach(&metadataTier_->callSites, code.callSites, callSiteOp)) {
    return false;
  }

  if (!callSiteTargets_.appendAll(code.callSiteTargets)) {
    return false;
  }

  for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
    auto trapSiteOp = [=](uint32_t, TrapSite* ts) {
      ts->offsetBy(offsetInModule);
    };
    if (!AppendForEach(&metadataTier_->trapSites[trap], code.trapSites[trap],
                       trapSiteOp)) {
      return false;
    }
  }

  for (const SymbolicAccess& access : code.symbolicAccesses) {
    uint32_t patchAt = offsetInModule + access.patchAt.offset();
    if (!linkData_->symbolicLinks[access.target].append(patchAt)) {
      return false;
    }
  }

  for (const CodeLabel& codeLabel : code.codeLabels) {
    LinkData::InternalLink link;
    link.patchAtOffset = offsetInModule + codeLabel.patchAt().offset();
    link.targetOffset = offsetInModule + codeLabel.target().offset();
#ifdef JS_CODELABEL_LINKMODE
    link.mode = codeLabel.linkMode();
#endif
    if (!linkData_->internalLinks.append(link)) {
      return false;
    }
  }

  for (size_t i = 0; i < code.stackMaps.length(); i++) {
    StackMaps::Maplet maplet = code.stackMaps.move(i);
    maplet.offsetBy(offsetInModule);
    if (!metadataTier_->stackMaps.add(maplet)) {
      // This function is now the only owner of maplet.map, so we'd better
      // free it right now.
      maplet.map->destroy();
      return false;
    }
  }

  auto unwindInfoOp = [=](uint32_t, CodeRangeUnwindInfo* i) {
    i->offsetBy(offsetInModule);
  };
  if (!AppendForEach(&metadataTier_->codeRangeUnwindInfos,
                     code.codeRangeUnwindInfos, unwindInfoOp)) {
    return false;
  }

  auto tryNoteFilter = [](const TryNote* tn) {
    // Filter out all try notes that were never given a try body. This may
    // happen due to dead code elimination.
    return tn->hasTryBody();
  };
  auto tryNoteOp = [=](uint32_t, TryNote* tn) { tn->offsetBy(offsetInModule); };
  return AppendForEach(&metadataTier_->tryNotes, code.tryNotes, tryNoteFilter,
                       tryNoteOp);
}

static bool ExecuteCompileTask(CompileTask* task, UniqueChars* error) {
  MOZ_ASSERT(task->lifo.isEmpty());
  MOZ_ASSERT(task->output.empty());

  switch (task->compilerEnv.tier()) {
    case Tier::Optimized:
      if (!IonCompileFunctions(task->moduleEnv, task->compilerEnv, task->lifo,
                               task->inputs, &task->output, error)) {
        return false;
      }
      break;
    case Tier::Baseline:
      if (!BaselineCompileFunctions(task->moduleEnv, task->compilerEnv,
                                    task->lifo, task->inputs, &task->output,
                                    error)) {
        return false;
      }
      break;
  }

  MOZ_ASSERT(task->lifo.isEmpty());
  MOZ_ASSERT(task->inputs.length() == task->output.codeRanges.length());
  task->inputs.clear();
  return true;
}

void CompileTask::runHelperThreadTask(AutoLockHelperThreadState& lock) {
  UniqueChars error;
  bool ok;

  {
    AutoUnlockHelperThreadState unlock(lock);
    ok = ExecuteCompileTask(this, &error);
  }

  // Don't release the lock between updating our state and returning from this
  // method.

  if (!ok || !state.finished().append(this)) {
    state.numFailed()++;
    if (!state.errorMessage()) {
      state.errorMessage() = std::move(error);
    }
  }

  state.condVar().notify_one(); /* failed or finished */
}

ThreadType CompileTask::threadType() {
  switch (compilerEnv.mode()) {
    case CompileMode::Once:
    case CompileMode::Tier1:
      return ThreadType::THREAD_TYPE_WASM_COMPILE_TIER1;
    case CompileMode::Tier2:
      return ThreadType::THREAD_TYPE_WASM_COMPILE_TIER2;
    default:
      MOZ_CRASH();
  }
}

bool ModuleGenerator::locallyCompileCurrentTask() {
  if (!ExecuteCompileTask(currentTask_, error_)) {
    return false;
  }
  if (!finishTask(currentTask_)) {
    return false;
  }
  currentTask_ = nullptr;
  batchedBytecode_ = 0;
  return true;
}

bool ModuleGenerator::finishTask(CompileTask* task) {
  AutoCreatedBy acb(masm_, "ModuleGenerator::finishTask");

  masm_.haltingAlign(CodeAlignment);

  if (!linkCompiledCode(task->output)) {
    return false;
  }

  task->output.clear();

  MOZ_ASSERT(task->inputs.empty());
  MOZ_ASSERT(task->output.empty());
  MOZ_ASSERT(task->lifo.isEmpty());
  freeTasks_.infallibleAppend(task);
  return true;
}

bool ModuleGenerator::launchBatchCompile() {
  MOZ_ASSERT(currentTask_);

  if (cancelled_ && *cancelled_) {
    return false;
  }

  if (!parallel_) {
    return locallyCompileCurrentTask();
  }

  if (!StartOffThreadWasmCompile(currentTask_, mode())) {
    return false;
  }
  outstanding_++;
  currentTask_ = nullptr;
  batchedBytecode_ = 0;
  return true;
}

bool ModuleGenerator::finishOutstandingTask() {
  MOZ_ASSERT(parallel_);

  CompileTask* task = nullptr;
  {
    AutoLockHelperThreadState lock;
    while (true) {
      MOZ_ASSERT(outstanding_ > 0);

      if (taskState_.numFailed() > 0) {
        return false;
      }

      if (!taskState_.finished().empty()) {
        outstanding_--;
        task = taskState_.finished().popCopy();
        break;
      }

      taskState_.condVar().wait(lock); /* failed or finished */
    }
  }

  // Call outside of the compilation lock.
  return finishTask(task);
}

bool ModuleGenerator::compileFuncDef(uint32_t funcIndex,
                                     uint32_t lineOrBytecode,
                                     const uint8_t* begin, const uint8_t* end,
                                     Uint32Vector&& lineNums) {
  MOZ_ASSERT(!finishedFuncDefs_);
  MOZ_ASSERT(funcIndex < moduleEnv_->numFuncs());

  uint32_t threshold;
  switch (tier()) {
    case Tier::Baseline:
      threshold = JitOptions.wasmBatchBaselineThreshold;
      break;
    case Tier::Optimized:
      threshold = JitOptions.wasmBatchIonThreshold;
      break;
    default:
      MOZ_CRASH("Invalid tier value");
      break;
  }

  uint32_t funcBytecodeLength = end - begin;

  // Do not go over the threshold if we can avoid it: spin off the compilation
  // before appending the function if we would go over.  (Very large single
  // functions may still exceed the threshold but this is fine; it'll be very
  // uncommon and is in any case safely handled by the MacroAssembler's buffer
  // limit logic.)

  if (currentTask_ && currentTask_->inputs.length() &&
      batchedBytecode_ + funcBytecodeLength > threshold) {
    if (!launchBatchCompile()) {
      return false;
    }
  }

  if (!currentTask_) {
    if (freeTasks_.empty() && !finishOutstandingTask()) {
      return false;
    }
    currentTask_ = freeTasks_.popCopy();
  }

  if (!currentTask_->inputs.emplaceBack(funcIndex, lineOrBytecode, begin, end,
                                        std::move(lineNums))) {
    return false;
  }

  batchedBytecode_ += funcBytecodeLength;
  MOZ_ASSERT(batchedBytecode_ <= MaxCodeSectionBytes);
  return true;
}

bool ModuleGenerator::finishFuncDefs() {
  MOZ_ASSERT(!finishedFuncDefs_);

  if (currentTask_ && !locallyCompileCurrentTask()) {
    return false;
  }

  finishedFuncDefs_ = true;
  return true;
}

bool ModuleGenerator::finishCodegen() {
  // Now that all functions and stubs are generated and their CodeRanges
  // known, patch all calls (which can emit far jumps) and far jumps. Linking
  // can emit tiny far-jump stubs, so there is an ordering dependency here.

  if (!linkCallSites()) {
    return false;
  }

  for (CallFarJump far : callFarJumps_) {
    masm_.patchFarJump(far.jump,
                       funcCodeRange(far.funcIndex).funcUncheckedCallEntry());
  }

  metadataTier_->debugTrapOffset = debugTrapCodeOffset_;

  // None of the linking or far-jump operations should emit masm metadata.

  MOZ_ASSERT(masm_.callSites().empty());
  MOZ_ASSERT(masm_.callSiteTargets().empty());
  MOZ_ASSERT(masm_.trapSites().empty());
  MOZ_ASSERT(masm_.symbolicAccesses().empty());
  MOZ_ASSERT(masm_.tryNotes().empty());
  MOZ_ASSERT(masm_.codeLabels().empty());

  masm_.finish();
  return !masm_.oom();
}

bool ModuleGenerator::finishMetadataTier() {
  // The stackmaps aren't yet sorted.  Do so now, since we'll need to
  // binary-search them at GC time.
  metadataTier_->stackMaps.finishAndSort();

  // The try notes also need to be sorted to simplify lookup.
  std::sort(metadataTier_->tryNotes.begin(), metadataTier_->tryNotes.end());

#ifdef DEBUG
  // Check that the stackmap contains no duplicates, since that could lead to
  // ambiguities about stack slot pointerness.
  const uint8_t* previousNextInsnAddr = nullptr;
  for (size_t i = 0; i < metadataTier_->stackMaps.length(); i++) {
    const StackMaps::Maplet& maplet = metadataTier_->stackMaps.get(i);
    MOZ_ASSERT_IF(i > 0, uintptr_t(maplet.nextInsnAddr) >
                             uintptr_t(previousNextInsnAddr));
    previousNextInsnAddr = maplet.nextInsnAddr;
  }

  // Assert all sorted metadata is sorted.
  uint32_t last = 0;
  for (const CodeRange& codeRange : metadataTier_->codeRanges) {
    MOZ_ASSERT(codeRange.begin() >= last);
    last = codeRange.end();
  }

  last = 0;
  for (const CallSite& callSite : metadataTier_->callSites) {
    MOZ_ASSERT(callSite.returnAddressOffset() >= last);
    last = callSite.returnAddressOffset();
  }

  for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
    last = 0;
    for (const TrapSite& trapSite : metadataTier_->trapSites[trap]) {
      MOZ_ASSERT(trapSite.pcOffset >= last);
      last = trapSite.pcOffset;
    }
  }

  last = 0;
  for (const CodeRangeUnwindInfo& info : metadataTier_->codeRangeUnwindInfos) {
    MOZ_ASSERT(info.offset() >= last);
    last = info.offset();
  }

  // Try notes should be sorted so that the end of ranges are in rising order
  // so that the innermost catch handler is chosen.
  last = 0;
  for (const TryNote& tryNote : metadataTier_->tryNotes) {
    MOZ_ASSERT(tryNote.tryBodyEnd() >= last);
    MOZ_ASSERT(tryNote.tryBodyEnd() > tryNote.tryBodyBegin());
    last = tryNote.tryBodyBegin();
  }
#endif

  // These Vectors can get large and the excess capacity can be significant,
  // so realloc them down to size.

  metadataTier_->funcToCodeRange.shrinkStorageToFit();
  metadataTier_->codeRanges.shrinkStorageToFit();
  metadataTier_->callSites.shrinkStorageToFit();
  metadataTier_->trapSites.shrinkStorageToFit();
  metadataTier_->tryNotes.shrinkStorageToFit();
  for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
    metadataTier_->trapSites[trap].shrinkStorageToFit();
  }

  return true;
}

UniqueCodeTier ModuleGenerator::finishCodeTier() {
  MOZ_ASSERT(finishedFuncDefs_);

  while (outstanding_ > 0) {
    if (!finishOutstandingTask()) {
      return nullptr;
    }
  }

#ifdef DEBUG
  for (uint32_t codeRangeIndex : metadataTier_->funcToCodeRange) {
    MOZ_ASSERT(codeRangeIndex != BAD_CODE_RANGE);
  }
#endif

  // Now that all imports/exports are known, we can generate a special
  // CompiledCode containing stubs.

  CompiledCode& stubCode = tasks_[0].output;
  MOZ_ASSERT(stubCode.empty());

  if (!GenerateStubs(*moduleEnv_, metadataTier_->funcImports,
                     metadataTier_->funcExports, &stubCode)) {
    return nullptr;
  }

  if (!linkCompiledCode(stubCode)) {
    return nullptr;
  }

  // Finish linking and metadata.

  if (!finishCodegen()) {
    return nullptr;
  }

  if (!finishMetadataTier()) {
    return nullptr;
  }

  UniqueModuleSegment segment =
      ModuleSegment::create(tier(), masm_, *linkData_);
  if (!segment) {
    warnf("failed to allocate executable memory for module");
    return nullptr;
  }

  metadataTier_->stackMaps.offsetBy(uintptr_t(segment->base()));

#if defined(DEBUG)
  // Check that each stackmap is associated with a plausible instruction.
  for (size_t i = 0; i < metadataTier_->stackMaps.length(); i++) {
    MOZ_ASSERT(
        IsPlausibleStackMapKey(metadataTier_->stackMaps.get(i).nextInsnAddr),
        "wasm stackmap does not reference a valid insn");
  }
#endif

#if defined(DEBUG) && (defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) ||   \
                       defined(JS_CODEGEN_ARM64) || defined(JS_CODEGEN_ARM) || \
                       defined(JS_CODEGEN_LOONG64))
  // Check that each trapsite is associated with a plausible instruction.  The
  // required instruction kind depends on the trapsite kind.
  //
  // NOTE: currently only enabled on x86_{32,64} and arm{32,64}.  Ideally it
  // should be extended to riscv, loongson, mips.
  //
  for (Trap trap : MakeEnumeratedRange(Trap::Limit)) {
    const TrapSiteVector& trapSites = metadataTier_->trapSites[trap];
    for (const TrapSite& trapSite : trapSites) {
      const uint8_t* insnAddr =
          ((const uint8_t*)(segment->base())) + uintptr_t(trapSite.pcOffset);
      // `expected` describes the kind of instruction we expect to see at
      // `insnAddr`.  Find out what is actually there and check it matches.
      const TrapMachineInsn expected = trapSite.insn;
      mozilla::Maybe<TrapMachineInsn> actual =
          SummarizeTrapInstruction(insnAddr);
      bool valid = actual.isSome() && actual.value() == expected;
      // This is useful for diagnosing validation failures.
      // if (!valid) {
      //   fprintf(stderr,
      //           "FAIL: reason=%-22s  expected=%-12s  "
      //           "pcOffset=%-5u  addr= %p\n",
      //           NameOfTrap(trap), NameOfTrapMachineInsn(expected),
      //           trapSite.pcOffset, insnAddr);
      //   if (actual.isSome()) {
      //     fprintf(stderr, "FAIL: identified as %s\n",
      //             actual.isSome() ? NameOfTrapMachineInsn(actual.value())
      //                             : "(insn not identified)");
      //   }
      // }
      MOZ_ASSERT(valid, "wasm trapsite does not reference a valid insn");
    }
  }
#endif

  return js::MakeUnique<CodeTier>(std::move(metadataTier_), std::move(segment));
}

SharedMetadata ModuleGenerator::finishMetadata(const Bytes& bytecode) {
  // Finish initialization of Metadata, which is only needed for constructing
  // the initial Module, not for tier-2 compilation.
  MOZ_ASSERT(mode() != CompileMode::Tier2);

  // Copy over data from the ModuleEnvironment.

  metadata_->startFuncIndex = moduleEnv_->startFuncIndex;
  metadata_->builtinModules = moduleEnv_->features.builtinModules;
  metadata_->memories = std::move(moduleEnv_->memories);
  metadata_->tables = std::move(moduleEnv_->tables);
  metadata_->globals = std::move(moduleEnv_->globals);
  metadata_->tags = std::move(moduleEnv_->tags);
  metadata_->nameCustomSectionIndex = moduleEnv_->nameCustomSectionIndex;
  metadata_->moduleName = moduleEnv_->moduleName;
  metadata_->funcNames = std::move(moduleEnv_->funcNames);
  metadata_->parsedBranchHints = moduleEnv_->parsedBranchHints;

  // Copy over additional debug information.

  if (compilerEnv_->debugEnabled()) {
    metadata_->debugEnabled = true;

    const size_t numFuncs = moduleEnv_->funcs.length();
    if (!metadata_->debugFuncTypeIndices.resize(numFuncs)) {
      return nullptr;
    }
    for (size_t i = 0; i < numFuncs; i++) {
      metadata_->debugFuncTypeIndices[i] = moduleEnv_->funcs[i].typeIndex;
    }

    static_assert(sizeof(ModuleHash) <= sizeof(mozilla::SHA1Sum::Hash),
                  "The ModuleHash size shall not exceed the SHA1 hash size.");
    mozilla::SHA1Sum::Hash hash;
    mozilla::SHA1Sum sha1Sum;
    sha1Sum.update(bytecode.begin(), bytecode.length());
    sha1Sum.finish(hash);
    memcpy(metadata_->debugHash, hash, sizeof(ModuleHash));
  }

  MOZ_ASSERT_IF(moduleEnv_->nameCustomSectionIndex, !!metadata_->namePayload);

  // Metadata shouldn't be mutably modified after finishMetadata().
  SharedMetadata metadata = metadata_;
  metadata_ = nullptr;
  return metadata;
}

SharedModule ModuleGenerator::finishModule(
    const ShareableBytes& bytecode,
    JS::OptimizedEncodingListener* maybeTier2Listener) {
  MOZ_ASSERT(mode() == CompileMode::Once || mode() == CompileMode::Tier1);

  UniqueCodeTier codeTier = finishCodeTier();
  if (!codeTier) {
    return nullptr;
  }

  JumpTables jumpTables;
  if (!jumpTables.init(mode(), codeTier->segment(),
                       codeTier->metadata().codeRanges)) {
    return nullptr;
  }

  // Copy over data from the Bytecode, which is going away at the end of
  // compilation.

  DataSegmentVector dataSegments;
  if (!dataSegments.reserve(moduleEnv_->dataSegments.length())) {
    return nullptr;
  }
  for (const DataSegmentEnv& srcSeg : moduleEnv_->dataSegments) {
    MutableDataSegment dstSeg = js_new<DataSegment>();
    if (!dstSeg) {
      return nullptr;
    }
    if (!dstSeg->init(bytecode, srcSeg)) {
      return nullptr;
    }
    dataSegments.infallibleAppend(std::move(dstSeg));
  }

  CustomSectionVector customSections;
  if (!customSections.reserve(moduleEnv_->customSections.length())) {
    return nullptr;
  }
  for (const CustomSectionEnv& srcSec : moduleEnv_->customSections) {
    CustomSection sec;
    if (!sec.name.append(bytecode.begin() + srcSec.nameOffset,
                         srcSec.nameLength)) {
      return nullptr;
    }
    MutableBytes payload = js_new<ShareableBytes>();
    if (!payload) {
      return nullptr;
    }
    if (!payload->append(bytecode.begin() + srcSec.payloadOffset,
                         srcSec.payloadLength)) {
      return nullptr;
    }
    sec.payload = std::move(payload);
    customSections.infallibleAppend(std::move(sec));
  }

  if (moduleEnv_->nameCustomSectionIndex) {
    metadata_->namePayload =
        customSections[*moduleEnv_->nameCustomSectionIndex].payload;
  }

  SharedMetadata metadata = finishMetadata(bytecode.bytes);
  if (!metadata) {
    return nullptr;
  }

  MutableCode code =
      js_new<Code>(std::move(codeTier), *metadata, std::move(jumpTables));
  if (!code || !code->initialize(*linkData_)) {
    return nullptr;
  }

  const ShareableBytes* debugBytecode = nullptr;
  if (compilerEnv_->debugEnabled()) {
    MOZ_ASSERT(mode() == CompileMode::Once);
    MOZ_ASSERT(tier() == Tier::Debug);
    debugBytecode = &bytecode;
  }

  // All the components are finished, so create the complete Module and start
  // tier-2 compilation if requested.

  MutableModule module = js_new<Module>(
      *code, std::move(moduleEnv_->imports), std::move(moduleEnv_->exports),
      std::move(dataSegments), std::move(moduleEnv_->elemSegments),
      std::move(customSections), debugBytecode);
  if (!module) {
    return nullptr;
  }

  if (!isAsmJS() && compileArgs_->features.testSerialization) {
    MOZ_RELEASE_ASSERT(mode() == CompileMode::Once &&
                       tier() == Tier::Serialized);

    Bytes serializedBytes;
    if (!module->serialize(*linkData_, &serializedBytes)) {
      return nullptr;
    }

    MutableModule deserializedModule =
        Module::deserialize(serializedBytes.begin(), serializedBytes.length());
    if (!deserializedModule) {
      return nullptr;
    }
    module = deserializedModule;

    // Perform storeOptimizedEncoding here instead of below so we don't have to
    // re-serialize the module.
    if (maybeTier2Listener) {
      maybeTier2Listener->storeOptimizedEncoding(serializedBytes.begin(),
                                                 serializedBytes.length());
      maybeTier2Listener = nullptr;
    }
  }

  if (mode() == CompileMode::Tier1) {
    module->startTier2(*compileArgs_, bytecode, maybeTier2Listener);
  } else if (tier() == Tier::Serialized && maybeTier2Listener) {
    Bytes bytes;
    if (module->serialize(*linkData_, &bytes)) {
      maybeTier2Listener->storeOptimizedEncoding(bytes.begin(), bytes.length());
    }
  }

  return module;
}

bool ModuleGenerator::finishTier2(const Module& module) {
  MOZ_ASSERT(mode() == CompileMode::Tier2);
  MOZ_ASSERT(tier() == Tier::Optimized);
  MOZ_ASSERT(!compilerEnv_->debugEnabled());

  if (cancelled_ && *cancelled_) {
    return false;
  }

  UniqueCodeTier codeTier = finishCodeTier();
  if (!codeTier) {
    return false;
  }

  if (MOZ_UNLIKELY(JitOptions.wasmDelayTier2)) {
    // Introduce an artificial delay when testing wasmDelayTier2, since we
    // want to exercise both tier1 and tier2 code in this case.
    ThisThread::SleepMilliseconds(500);
  }

  return module.finishTier2(*linkData_, std::move(codeTier));
}

void ModuleGenerator::warnf(const char* msg, ...) {
  if (!warnings_) {
    return;
  }

  va_list ap;
  va_start(ap, msg);
  UniqueChars str(JS_vsmprintf(msg, ap));
  va_end(ap);
  if (!str) {
    return;
  }

  (void)warnings_->append(std::move(str));
}

size_t CompiledCode::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  size_t trapSitesSize = 0;
  for (const TrapSiteVector& vec : trapSites) {
    trapSitesSize += vec.sizeOfExcludingThis(mallocSizeOf);
  }

  return bytes.sizeOfExcludingThis(mallocSizeOf) +
         codeRanges.sizeOfExcludingThis(mallocSizeOf) +
         callSites.sizeOfExcludingThis(mallocSizeOf) +
         callSiteTargets.sizeOfExcludingThis(mallocSizeOf) + trapSitesSize +
         symbolicAccesses.sizeOfExcludingThis(mallocSizeOf) +
         tryNotes.sizeOfExcludingThis(mallocSizeOf) +
         codeLabels.sizeOfExcludingThis(mallocSizeOf);
}

size_t CompileTask::sizeOfExcludingThis(
    mozilla::MallocSizeOf mallocSizeOf) const {
  return lifo.sizeOfExcludingThis(mallocSizeOf) +
         inputs.sizeOfExcludingThis(mallocSizeOf) +
         output.sizeOfExcludingThis(mallocSizeOf);
}
