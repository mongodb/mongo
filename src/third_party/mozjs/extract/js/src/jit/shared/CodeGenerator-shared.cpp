/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/CodeGenerator-shared-inl.h"

#include "mozilla/DebugOnly.h"

#include <utility>

#include "jit/CodeGenerator.h"
#include "jit/CompactBuffer.h"
#include "jit/CompileInfo.h"
#include "jit/InlineScriptTree.h"
#include "jit/JitcodeMap.h"
#include "jit/JitFrames.h"
#include "jit/JitSpewer.h"
#include "jit/MacroAssembler.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/SafepointIndex.h"
#include "js/Conversions.h"
#include "util/Memory.h"
#include "vm/TraceLogging.h"

#include "jit/MacroAssembler-inl.h"
#include "vm/JSScript-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::BitwiseCast;
using mozilla::DebugOnly;

namespace js {
namespace jit {

MacroAssembler& CodeGeneratorShared::ensureMasm(MacroAssembler* masmArg) {
  if (masmArg) {
    return *masmArg;
  }
  maybeMasm_.emplace();
  return *maybeMasm_;
}

CodeGeneratorShared::CodeGeneratorShared(MIRGenerator* gen, LIRGraph* graph,
                                         MacroAssembler* masmArg)
    : maybeMasm_(),
      useWasmStackArgumentAbi_(false),
      masm(ensureMasm(masmArg)),
      gen(gen),
      graph(*graph),
      current(nullptr),
      snapshots_(),
      recovers_(),
      deoptTable_(),
#ifdef DEBUG
      pushedArgs_(0),
#endif
      lastOsiPointOffset_(0),
      safepoints_(graph->totalSlotCount(),
                  (gen->outerInfo().nargs() + 1) * sizeof(Value)),
      returnLabel_(),
      nativeToBytecodeMap_(nullptr),
      nativeToBytecodeMapSize_(0),
      nativeToBytecodeTableOffset_(0),
      nativeToBytecodeNumRegions_(0),
      nativeToBytecodeScriptList_(nullptr),
      nativeToBytecodeScriptListLength_(0),
#ifdef CHECK_OSIPOINT_REGISTERS
      checkOsiPointRegisters(JitOptions.checkOsiPointRegisters),
#endif
      frameDepth_(graph->paddedLocalSlotsSize() + graph->argumentsSize()),
      frameClass_(FrameSizeClass::None()) {
  if (gen->isProfilerInstrumentationEnabled()) {
    masm.enableProfilingInstrumentation();
  }

  if (gen->compilingWasm()) {
    // Since wasm uses the system ABI which does not necessarily use a
    // regular array where all slots are sizeof(Value), it maintains the max
    // argument stack depth separately.
    MOZ_ASSERT(graph->argumentSlotCount() == 0);
    frameDepth_ += gen->wasmMaxStackArgBytes();

#ifdef ENABLE_WASM_SIMD
#  if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86) || \
      defined(JS_CODEGEN_ARM64)
    // On X64/x86 and ARM64, we don't need alignment for Wasm SIMD at this time.
#  else
#    error \
        "we may need padding so that local slots are SIMD-aligned and the stack must be kept SIMD-aligned too."
#  endif
#endif

    if (gen->needsStaticStackAlignment()) {
      // An MWasmCall does not align the stack pointer at calls sites but
      // instead relies on the a priori stack adjustment. This must be the
      // last adjustment of frameDepth_.
      frameDepth_ += ComputeByteAlignment(sizeof(wasm::Frame) + frameDepth_,
                                          WasmStackAlignment);
    }

    // FrameSizeClass is only used for bailing, which cannot happen in
    // wasm code.
    MOZ_ASSERT(frameClass_ == FrameSizeClass::None());
  } else {
    frameClass_ = FrameSizeClass::FromDepth(frameDepth_);
  }
}

bool CodeGeneratorShared::generatePrologue() {
  MOZ_ASSERT(masm.framePushed() == 0);
  MOZ_ASSERT(!gen->compilingWasm());

#ifdef JS_USE_LINK_REGISTER
  masm.pushReturnAddress();
#endif

  // If profiling, save the current frame pointer to a per-thread global field.
  if (isProfilerInstrumentationEnabled()) {
    masm.profilerEnterFrame(masm.getStackPointer(), CallTempReg0);
  }

  // Ensure that the Ion frame is properly aligned.
  masm.assertStackAlignment(JitStackAlignment, 0);

  // Note that this automatically sets MacroAssembler::framePushed().
  masm.reserveStack(frameSize());
  masm.checkStackAlignment();

  if (JS::TraceLoggerSupported()) {
    emitTracelogIonStart();
  }

  return true;
}

bool CodeGeneratorShared::generateEpilogue() {
  MOZ_ASSERT(!gen->compilingWasm());
  masm.bind(&returnLabel_);

  if (JS::TraceLoggerSupported()) {
    emitTracelogIonStop();
  }

  masm.freeStack(frameSize());
  MOZ_ASSERT(masm.framePushed() == 0);

  // If profiling, reset the per-thread global lastJitFrame to point to
  // the previous frame.
  if (isProfilerInstrumentationEnabled()) {
    masm.profilerExitFrame();
  }

  masm.ret();

  // On systems that use a constant pool, this is a good time to emit.
  masm.flushBuffer();
  return true;
}

bool CodeGeneratorShared::generateOutOfLineCode() {
  // OOL paths should not attempt to use |current| as it's the last block
  // instead of the block corresponding to the OOL path.
  current = nullptr;

  for (size_t i = 0; i < outOfLineCode_.length(); i++) {
    // Add native => bytecode mapping entries for OOL sites.
    // Not enabled on wasm yet since it doesn't contain bytecode mappings.
    if (!gen->compilingWasm()) {
      if (!addNativeToBytecodeEntry(outOfLineCode_[i]->bytecodeSite())) {
        return false;
      }
    }

    if (!gen->alloc().ensureBallast()) {
      return false;
    }

    JitSpew(JitSpew_Codegen, "# Emitting out of line code");

    masm.setFramePushed(outOfLineCode_[i]->framePushed());
    outOfLineCode_[i]->bind(&masm);

    outOfLineCode_[i]->generate(this);
  }

  return !masm.oom();
}

void CodeGeneratorShared::addOutOfLineCode(OutOfLineCode* code,
                                           const MInstruction* mir) {
  MOZ_ASSERT(mir);
  addOutOfLineCode(code, mir->trackedSite());
}

void CodeGeneratorShared::addOutOfLineCode(OutOfLineCode* code,
                                           const BytecodeSite* site) {
  MOZ_ASSERT_IF(!gen->compilingWasm(), site->script()->containsPC(site->pc()));
  code->setFramePushed(masm.framePushed());
  code->setBytecodeSite(site);
  masm.propagateOOM(outOfLineCode_.append(code));
}

bool CodeGeneratorShared::addNativeToBytecodeEntry(const BytecodeSite* site) {
  MOZ_ASSERT(site);
  MOZ_ASSERT(site->tree());
  MOZ_ASSERT(site->pc());

  // Skip the table entirely if profiling is not enabled.
  if (!isProfilerInstrumentationEnabled()) {
    return true;
  }

  // Fails early if the last added instruction caused the macro assembler to
  // run out of memory as continuity assumption below do not hold.
  if (masm.oom()) {
    return false;
  }

  InlineScriptTree* tree = site->tree();
  jsbytecode* pc = site->pc();
  uint32_t nativeOffset = masm.currentOffset();

  MOZ_ASSERT_IF(nativeToBytecodeList_.empty(), nativeOffset == 0);

  if (!nativeToBytecodeList_.empty()) {
    size_t lastIdx = nativeToBytecodeList_.length() - 1;
    NativeToBytecode& lastEntry = nativeToBytecodeList_[lastIdx];

    MOZ_ASSERT(nativeOffset >= lastEntry.nativeOffset.offset());

    // If the new entry is for the same inlineScriptTree and same
    // bytecodeOffset, but the nativeOffset has changed, do nothing.
    // The same site just generated some more code.
    if (lastEntry.tree == tree && lastEntry.pc == pc) {
      JitSpew(JitSpew_Profiling, " => In-place update [%zu-%" PRIu32 "]",
              lastEntry.nativeOffset.offset(), nativeOffset);
      return true;
    }

    // If the new entry is for the same native offset, then update the
    // previous entry with the new bytecode site, since the previous
    // bytecode site did not generate any native code.
    if (lastEntry.nativeOffset.offset() == nativeOffset) {
      lastEntry.tree = tree;
      lastEntry.pc = pc;
      JitSpew(JitSpew_Profiling, " => Overwriting zero-length native region.");

      // This overwrite might have made the entry merge-able with a
      // previous one.  If so, merge it.
      if (lastIdx > 0) {
        NativeToBytecode& nextToLastEntry = nativeToBytecodeList_[lastIdx - 1];
        if (nextToLastEntry.tree == lastEntry.tree &&
            nextToLastEntry.pc == lastEntry.pc) {
          JitSpew(JitSpew_Profiling, " => Merging with previous region");
          nativeToBytecodeList_.erase(&lastEntry);
        }
      }

      dumpNativeToBytecodeEntry(nativeToBytecodeList_.length() - 1);
      return true;
    }
  }

  // Otherwise, some native code was generated for the previous bytecode site.
  // Add a new entry for code that is about to be generated.
  NativeToBytecode entry;
  entry.nativeOffset = CodeOffset(nativeOffset);
  entry.tree = tree;
  entry.pc = pc;
  if (!nativeToBytecodeList_.append(entry)) {
    return false;
  }

  JitSpew(JitSpew_Profiling, " => Push new entry.");
  dumpNativeToBytecodeEntry(nativeToBytecodeList_.length() - 1);
  return true;
}

void CodeGeneratorShared::dumpNativeToBytecodeEntries() {
#ifdef JS_JITSPEW
  InlineScriptTree* topTree = gen->outerInfo().inlineScriptTree();
  JitSpewStart(JitSpew_Profiling, "Native To Bytecode Entries for %s:%u:%u\n",
               topTree->script()->filename(), topTree->script()->lineno(),
               topTree->script()->column());
  for (unsigned i = 0; i < nativeToBytecodeList_.length(); i++) {
    dumpNativeToBytecodeEntry(i);
  }
#endif
}

void CodeGeneratorShared::dumpNativeToBytecodeEntry(uint32_t idx) {
#ifdef JS_JITSPEW
  NativeToBytecode& ref = nativeToBytecodeList_[idx];
  InlineScriptTree* tree = ref.tree;
  JSScript* script = tree->script();
  uint32_t nativeOffset = ref.nativeOffset.offset();
  unsigned nativeDelta = 0;
  unsigned pcDelta = 0;
  if (idx + 1 < nativeToBytecodeList_.length()) {
    NativeToBytecode* nextRef = &ref + 1;
    nativeDelta = nextRef->nativeOffset.offset() - nativeOffset;
    if (nextRef->tree == ref.tree) {
      pcDelta = nextRef->pc - ref.pc;
    }
  }
  JitSpewStart(
      JitSpew_Profiling, "    %08zx [+%-6u] => %-6ld [%-4u] {%-10s} (%s:%u:%u",
      ref.nativeOffset.offset(), nativeDelta, (long)(ref.pc - script->code()),
      pcDelta, CodeName(JSOp(*ref.pc)), script->filename(), script->lineno(),
      script->column());

  for (tree = tree->caller(); tree; tree = tree->caller()) {
    JitSpewCont(JitSpew_Profiling, " <= %s:%u:%u", tree->script()->filename(),
                tree->script()->lineno(), tree->script()->column());
  }
  JitSpewCont(JitSpew_Profiling, ")");
  JitSpewFin(JitSpew_Profiling);
#endif
}

// see OffsetOfFrameSlot
static inline int32_t ToStackIndex(LAllocation* a) {
  if (a->isStackSlot()) {
    MOZ_ASSERT(a->toStackSlot()->slot() >= 1);
    return a->toStackSlot()->slot();
  }
  return -int32_t(sizeof(JitFrameLayout) + a->toArgument()->index());
}

void CodeGeneratorShared::encodeAllocation(LSnapshot* snapshot,
                                           MDefinition* mir,
                                           uint32_t* allocIndex) {
  if (mir->isBox()) {
    mir = mir->toBox()->getOperand(0);
  }

  MIRType type = mir->isRecoveredOnBailout() ? MIRType::None
                 : mir->isUnused()           ? MIRType::MagicOptimizedOut
                                             : mir->type();

  RValueAllocation alloc;

  switch (type) {
    case MIRType::None: {
      MOZ_ASSERT(mir->isRecoveredOnBailout());
      uint32_t index = 0;
      LRecoverInfo* recoverInfo = snapshot->recoverInfo();
      MNode** it = recoverInfo->begin();
      MNode** end = recoverInfo->end();
      while (it != end && mir != *it) {
        ++it;
        ++index;
      }

      // This MDefinition is recovered, thus it should be listed in the
      // LRecoverInfo.
      MOZ_ASSERT(it != end && mir == *it);

      // Lambda should have a default value readable for iterating over the
      // inner frames.
      MConstant* functionOperand = nullptr;
      if (mir->isLambda()) {
        functionOperand = mir->toLambda()->functionOperand();
      } else if (mir->isLambdaArrow()) {
        functionOperand = mir->toLambdaArrow()->functionOperand();
      } else if (mir->isFunctionWithProto()) {
        functionOperand = mir->toFunctionWithProto()->functionOperand();
      }
      if (functionOperand) {
        uint32_t cstIndex;
        masm.propagateOOM(
            graph.addConstantToPool(functionOperand->toJSValue(), &cstIndex));
        alloc = RValueAllocation::RecoverInstruction(index, cstIndex);
        break;
      }

      alloc = RValueAllocation::RecoverInstruction(index);
      break;
    }
    case MIRType::Undefined:
      alloc = RValueAllocation::Undefined();
      break;
    case MIRType::Null:
      alloc = RValueAllocation::Null();
      break;
    case MIRType::Int32:
    case MIRType::String:
    case MIRType::Symbol:
    case MIRType::BigInt:
    case MIRType::Object:
    case MIRType::Shape:
    case MIRType::Boolean:
    case MIRType::Double: {
      LAllocation* payload = snapshot->payloadOfSlot(*allocIndex);
      if (payload->isConstant()) {
        MConstant* constant = mir->toConstant();
        uint32_t index;
        masm.propagateOOM(
            graph.addConstantToPool(constant->toJSValue(), &index));
        alloc = RValueAllocation::ConstantPool(index);
        break;
      }

      JSValueType valueType = ValueTypeFromMIRType(type);

      MOZ_DIAGNOSTIC_ASSERT(payload->isMemory() || payload->isRegister());
      if (payload->isMemory()) {
        alloc = RValueAllocation::Typed(valueType, ToStackIndex(payload));
      } else if (payload->isGeneralReg()) {
        alloc = RValueAllocation::Typed(valueType, ToRegister(payload));
      } else if (payload->isFloatReg()) {
        alloc = RValueAllocation::Double(ToFloatRegister(payload));
      } else {
        MOZ_CRASH("Unexpected payload type.");
      }
      break;
    }
    case MIRType::Float32:
    case MIRType::Simd128: {
      LAllocation* payload = snapshot->payloadOfSlot(*allocIndex);
      if (payload->isConstant()) {
        MConstant* constant = mir->toConstant();
        uint32_t index;
        masm.propagateOOM(
            graph.addConstantToPool(constant->toJSValue(), &index));
        alloc = RValueAllocation::ConstantPool(index);
        break;
      }

      MOZ_ASSERT(payload->isMemory() || payload->isFloatReg());
      if (payload->isFloatReg()) {
        alloc = RValueAllocation::AnyFloat(ToFloatRegister(payload));
      } else {
        alloc = RValueAllocation::AnyFloat(ToStackIndex(payload));
      }
      break;
    }
    case MIRType::MagicOptimizedOut:
    case MIRType::MagicUninitializedLexical:
    case MIRType::MagicIsConstructing: {
      uint32_t index;
      JSWhyMagic why = JS_GENERIC_MAGIC;
      switch (type) {
        case MIRType::MagicOptimizedOut:
          why = JS_OPTIMIZED_OUT;
          break;
        case MIRType::MagicUninitializedLexical:
          why = JS_UNINITIALIZED_LEXICAL;
          break;
        case MIRType::MagicIsConstructing:
          why = JS_IS_CONSTRUCTING;
          break;
        default:
          MOZ_CRASH("Invalid Magic MIRType");
      }

      Value v = MagicValue(why);
      masm.propagateOOM(graph.addConstantToPool(v, &index));
      alloc = RValueAllocation::ConstantPool(index);
      break;
    }
    default: {
      MOZ_ASSERT(mir->type() == MIRType::Value);
      LAllocation* payload = snapshot->payloadOfSlot(*allocIndex);
#ifdef JS_NUNBOX32
      LAllocation* type = snapshot->typeOfSlot(*allocIndex);
      if (type->isRegister()) {
        if (payload->isRegister()) {
          alloc =
              RValueAllocation::Untyped(ToRegister(type), ToRegister(payload));
        } else {
          alloc = RValueAllocation::Untyped(ToRegister(type),
                                            ToStackIndex(payload));
        }
      } else {
        if (payload->isRegister()) {
          alloc = RValueAllocation::Untyped(ToStackIndex(type),
                                            ToRegister(payload));
        } else {
          alloc = RValueAllocation::Untyped(ToStackIndex(type),
                                            ToStackIndex(payload));
        }
      }
#elif JS_PUNBOX64
      if (payload->isRegister()) {
        alloc = RValueAllocation::Untyped(ToRegister(payload));
      } else {
        alloc = RValueAllocation::Untyped(ToStackIndex(payload));
      }
#endif
      break;
    }
  }
  MOZ_DIAGNOSTIC_ASSERT(alloc.valid());

  // This set an extra bit as part of the RValueAllocation, such that we know
  // that recover instruction have to be executed without wrapping the
  // instruction in a no-op recover instruction.
  if (mir->isIncompleteObject()) {
    alloc.setNeedSideEffect();
  }

  masm.propagateOOM(snapshots_.add(alloc));

  *allocIndex += mir->isRecoveredOnBailout() ? 0 : 1;
}

void CodeGeneratorShared::encode(LRecoverInfo* recover) {
  if (recover->recoverOffset() != INVALID_RECOVER_OFFSET) {
    return;
  }

  uint32_t numInstructions = recover->numInstructions();
  JitSpew(JitSpew_IonSnapshots,
          "Encoding LRecoverInfo %p (frameCount %u, instructions %u)",
          (void*)recover, recover->mir()->frameCount(), numInstructions);

  MResumePoint::Mode mode = recover->mir()->mode();
  MOZ_ASSERT(mode != MResumePoint::Outer);
  bool resumeAfter = (mode == MResumePoint::ResumeAfter);

  RecoverOffset offset = recovers_.startRecover(numInstructions, resumeAfter);

  for (MNode* insn : *recover) {
    recovers_.writeInstruction(insn);
  }

  recovers_.endRecover();
  recover->setRecoverOffset(offset);
  masm.propagateOOM(!recovers_.oom());
}

void CodeGeneratorShared::encode(LSnapshot* snapshot) {
  if (snapshot->snapshotOffset() != INVALID_SNAPSHOT_OFFSET) {
    return;
  }

  LRecoverInfo* recoverInfo = snapshot->recoverInfo();
  encode(recoverInfo);

  RecoverOffset recoverOffset = recoverInfo->recoverOffset();
  MOZ_ASSERT(recoverOffset != INVALID_RECOVER_OFFSET);

  JitSpew(JitSpew_IonSnapshots, "Encoding LSnapshot %p (LRecover %p)",
          (void*)snapshot, (void*)recoverInfo);

  SnapshotOffset offset =
      snapshots_.startSnapshot(recoverOffset, snapshot->bailoutKind());

#ifdef TRACK_SNAPSHOTS
  uint32_t pcOpcode = 0;
  uint32_t lirOpcode = 0;
  uint32_t lirId = 0;
  uint32_t mirOpcode = 0;
  uint32_t mirId = 0;

  if (LInstruction* ins = instruction()) {
    lirOpcode = uint32_t(ins->op());
    lirId = ins->id();
    if (MDefinition* mir = ins->mirRaw()) {
      mirOpcode = uint32_t(mir->op());
      mirId = mir->id();
      if (jsbytecode* pc = mir->trackedSite()->pc()) {
        pcOpcode = *pc;
      }
    }
  }
  snapshots_.trackSnapshot(pcOpcode, mirOpcode, mirId, lirOpcode, lirId);
#endif

  uint32_t allocIndex = 0;
  for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
    DebugOnly<uint32_t> allocWritten = snapshots_.allocWritten();
    encodeAllocation(snapshot, *it, &allocIndex);
    MOZ_ASSERT_IF(!snapshots_.oom(),
                  allocWritten + 1 == snapshots_.allocWritten());
  }

  MOZ_ASSERT(allocIndex == snapshot->numSlots());
  snapshots_.endSnapshot();
  snapshot->setSnapshotOffset(offset);
  masm.propagateOOM(!snapshots_.oom());
}

bool CodeGeneratorShared::assignBailoutId(LSnapshot* snapshot) {
  MOZ_ASSERT(snapshot->snapshotOffset() != INVALID_SNAPSHOT_OFFSET);

  // Can we not use bailout tables at all?
  if (!deoptTable_) {
    return false;
  }

  MOZ_ASSERT(frameClass_ != FrameSizeClass::None());

  if (snapshot->bailoutId() != INVALID_BAILOUT_ID) {
    return true;
  }

  // Is the bailout table full?
  if (bailouts_.length() >= BAILOUT_TABLE_SIZE) {
    return false;
  }

  unsigned bailoutId = bailouts_.length();
  snapshot->setBailoutId(bailoutId);
  JitSpew(JitSpew_IonSnapshots, "Assigned snapshot bailout id %u", bailoutId);
  masm.propagateOOM(bailouts_.append(snapshot->snapshotOffset()));
  return true;
}

bool CodeGeneratorShared::encodeSafepoints() {
  for (CodegenSafepointIndex& index : safepointIndices_) {
    LSafepoint* safepoint = index.safepoint();

    if (!safepoint->encoded()) {
      safepoints_.encode(safepoint);
    }
  }

  return !safepoints_.oom();
}

bool CodeGeneratorShared::createNativeToBytecodeScriptList(JSContext* cx) {
  js::Vector<JSScript*, 0, SystemAllocPolicy> scriptList;
  InlineScriptTree* tree = gen->outerInfo().inlineScriptTree();
  for (;;) {
    // Add script from current tree.
    bool found = false;
    for (uint32_t i = 0; i < scriptList.length(); i++) {
      if (scriptList[i] == tree->script()) {
        found = true;
        break;
      }
    }
    if (!found) {
      if (!scriptList.append(tree->script())) {
        return false;
      }
    }

    // Process rest of tree

    // If children exist, emit children.
    if (tree->hasChildren()) {
      tree = tree->firstChild();
      continue;
    }

    // Otherwise, find the first tree up the chain (including this one)
    // that contains a next sibling.
    while (!tree->hasNextCallee() && tree->hasCaller()) {
      tree = tree->caller();
    }

    // If we found a sibling, use it.
    if (tree->hasNextCallee()) {
      tree = tree->nextCallee();
      continue;
    }

    // Otherwise, we must have reached the top without finding any siblings.
    MOZ_ASSERT(tree->isOutermostCaller());
    break;
  }

  // Allocate array for list.
  JSScript** data = cx->pod_malloc<JSScript*>(scriptList.length());
  if (!data) {
    return false;
  }

  for (uint32_t i = 0; i < scriptList.length(); i++) {
    data[i] = scriptList[i];
  }

  // Success.
  nativeToBytecodeScriptListLength_ = scriptList.length();
  nativeToBytecodeScriptList_ = data;
  return true;
}

bool CodeGeneratorShared::generateCompactNativeToBytecodeMap(JSContext* cx,
                                                             JitCode* code) {
  MOZ_ASSERT(nativeToBytecodeScriptListLength_ == 0);
  MOZ_ASSERT(nativeToBytecodeScriptList_ == nullptr);
  MOZ_ASSERT(nativeToBytecodeMap_ == nullptr);
  MOZ_ASSERT(nativeToBytecodeMapSize_ == 0);
  MOZ_ASSERT(nativeToBytecodeTableOffset_ == 0);
  MOZ_ASSERT(nativeToBytecodeNumRegions_ == 0);

  if (!createNativeToBytecodeScriptList(cx)) {
    return false;
  }

  MOZ_ASSERT(nativeToBytecodeScriptListLength_ > 0);
  MOZ_ASSERT(nativeToBytecodeScriptList_ != nullptr);

  CompactBufferWriter writer;
  uint32_t tableOffset = 0;
  uint32_t numRegions = 0;

  if (!JitcodeIonTable::WriteIonTable(
          writer, nativeToBytecodeScriptList_,
          nativeToBytecodeScriptListLength_, &nativeToBytecodeList_[0],
          &nativeToBytecodeList_[0] + nativeToBytecodeList_.length(),
          &tableOffset, &numRegions)) {
    js_free(nativeToBytecodeScriptList_);
    return false;
  }

  MOZ_ASSERT(tableOffset > 0);
  MOZ_ASSERT(numRegions > 0);

  // Writer is done, copy it to sized buffer.
  uint8_t* data = cx->pod_malloc<uint8_t>(writer.length());
  if (!data) {
    js_free(nativeToBytecodeScriptList_);
    return false;
  }

  memcpy(data, writer.buffer(), writer.length());
  nativeToBytecodeMap_ = data;
  nativeToBytecodeMapSize_ = writer.length();
  nativeToBytecodeTableOffset_ = tableOffset;
  nativeToBytecodeNumRegions_ = numRegions;

  verifyCompactNativeToBytecodeMap(code);

  JitSpew(JitSpew_Profiling, "Compact Native To Bytecode Map [%p-%p]", data,
          data + nativeToBytecodeMapSize_);

  return true;
}

void CodeGeneratorShared::verifyCompactNativeToBytecodeMap(JitCode* code) {
#ifdef DEBUG
  MOZ_ASSERT(nativeToBytecodeScriptListLength_ > 0);
  MOZ_ASSERT(nativeToBytecodeScriptList_ != nullptr);
  MOZ_ASSERT(nativeToBytecodeMap_ != nullptr);
  MOZ_ASSERT(nativeToBytecodeMapSize_ > 0);
  MOZ_ASSERT(nativeToBytecodeTableOffset_ > 0);
  MOZ_ASSERT(nativeToBytecodeNumRegions_ > 0);

  // The pointer to the table must be 4-byte aligned
  const uint8_t* tablePtr = nativeToBytecodeMap_ + nativeToBytecodeTableOffset_;
  MOZ_ASSERT(uintptr_t(tablePtr) % sizeof(uint32_t) == 0);

  // Verify that numRegions was encoded correctly.
  const JitcodeIonTable* ionTable =
      reinterpret_cast<const JitcodeIonTable*>(tablePtr);
  MOZ_ASSERT(ionTable->numRegions() == nativeToBytecodeNumRegions_);

  // Region offset for first region should be at the start of the payload
  // region. Since the offsets are backward from the start of the table, the
  // first entry backoffset should be equal to the forward table offset from the
  // start of the allocated data.
  MOZ_ASSERT(ionTable->regionOffset(0) == nativeToBytecodeTableOffset_);

  // Verify each region.
  for (uint32_t i = 0; i < ionTable->numRegions(); i++) {
    // Back-offset must point into the payload region preceding the table, not
    // before it.
    MOZ_ASSERT(ionTable->regionOffset(i) <= nativeToBytecodeTableOffset_);

    // Back-offset must point to a later area in the payload region than
    // previous back-offset.  This means that back-offsets decrease
    // monotonically.
    MOZ_ASSERT_IF(i > 0,
                  ionTable->regionOffset(i) < ionTable->regionOffset(i - 1));

    JitcodeRegionEntry entry = ionTable->regionEntry(i);

    // Ensure native code offset for region falls within jitcode.
    MOZ_ASSERT(entry.nativeOffset() <= code->instructionsSize());

    // Read out script/pc stack and verify.
    JitcodeRegionEntry::ScriptPcIterator scriptPcIter =
        entry.scriptPcIterator();
    while (scriptPcIter.hasMore()) {
      uint32_t scriptIdx = 0, pcOffset = 0;
      scriptPcIter.readNext(&scriptIdx, &pcOffset);

      // Ensure scriptIdx refers to a valid script in the list.
      MOZ_ASSERT(scriptIdx < nativeToBytecodeScriptListLength_);
      JSScript* script = nativeToBytecodeScriptList_[scriptIdx];

      // Ensure pcOffset falls within the script.
      MOZ_ASSERT(pcOffset < script->length());
    }

    // Obtain the original nativeOffset and pcOffset and script.
    uint32_t curNativeOffset = entry.nativeOffset();
    JSScript* script = nullptr;
    uint32_t curPcOffset = 0;
    {
      uint32_t scriptIdx = 0;
      scriptPcIter.reset();
      scriptPcIter.readNext(&scriptIdx, &curPcOffset);
      script = nativeToBytecodeScriptList_[scriptIdx];
    }

    // Read out nativeDeltas and pcDeltas and verify.
    JitcodeRegionEntry::DeltaIterator deltaIter = entry.deltaIterator();
    while (deltaIter.hasMore()) {
      uint32_t nativeDelta = 0;
      int32_t pcDelta = 0;
      deltaIter.readNext(&nativeDelta, &pcDelta);

      curNativeOffset += nativeDelta;
      curPcOffset = uint32_t(int32_t(curPcOffset) + pcDelta);

      // Ensure that nativeOffset still falls within jitcode after delta.
      MOZ_ASSERT(curNativeOffset <= code->instructionsSize());

      // Ensure that pcOffset still falls within bytecode after delta.
      MOZ_ASSERT(curPcOffset < script->length());
    }
  }
#endif  // DEBUG
}

void CodeGeneratorShared::markSafepoint(LInstruction* ins) {
  markSafepointAt(masm.currentOffset(), ins);
}

void CodeGeneratorShared::markSafepointAt(uint32_t offset, LInstruction* ins) {
  MOZ_ASSERT_IF(
      !safepointIndices_.empty() && !masm.oom(),
      offset - safepointIndices_.back().displacement() >= sizeof(uint32_t));
  masm.propagateOOM(safepointIndices_.append(
      CodegenSafepointIndex(offset, ins->safepoint())));
}

void CodeGeneratorShared::ensureOsiSpace() {
  // For a refresher, an invalidation point is of the form:
  // 1: call <target>
  // 2: ...
  // 3: <osipoint>
  //
  // The four bytes *before* instruction 2 are overwritten with an offset.
  // Callers must ensure that the instruction itself has enough bytes to
  // support this.
  //
  // The bytes *at* instruction 3 are overwritten with an invalidation jump.
  // jump. These bytes may be in a completely different IR sequence, but
  // represent the join point of the call out of the function.
  //
  // At points where we want to ensure that invalidation won't corrupt an
  // important instruction, we make sure to pad with nops.
  if (masm.currentOffset() - lastOsiPointOffset_ <
      Assembler::PatchWrite_NearCallSize()) {
    int32_t paddingSize = Assembler::PatchWrite_NearCallSize();
    paddingSize -= masm.currentOffset() - lastOsiPointOffset_;
    for (int32_t i = 0; i < paddingSize; ++i) {
      masm.nop();
    }
  }
  MOZ_ASSERT_IF(!masm.oom(), masm.currentOffset() - lastOsiPointOffset_ >=
                                 Assembler::PatchWrite_NearCallSize());
  lastOsiPointOffset_ = masm.currentOffset();
}

uint32_t CodeGeneratorShared::markOsiPoint(LOsiPoint* ins) {
  encode(ins->snapshot());
  ensureOsiSpace();

  uint32_t offset = masm.currentOffset();
  SnapshotOffset so = ins->snapshot()->snapshotOffset();
  masm.propagateOOM(osiIndices_.append(OsiIndex(offset, so)));

  return offset;
}

class OutOfLineTruncateSlow : public OutOfLineCodeBase<CodeGeneratorShared> {
  FloatRegister src_;
  Register dest_;
  bool widenFloatToDouble_;
  wasm::BytecodeOffset bytecodeOffset_;
  bool preserveTls_;

 public:
  OutOfLineTruncateSlow(
      FloatRegister src, Register dest, bool widenFloatToDouble = false,
      wasm::BytecodeOffset bytecodeOffset = wasm::BytecodeOffset(),
      bool preserveTls = false)
      : src_(src),
        dest_(dest),
        widenFloatToDouble_(widenFloatToDouble),
        bytecodeOffset_(bytecodeOffset),
        preserveTls_(preserveTls) {}

  void accept(CodeGeneratorShared* codegen) override {
    codegen->visitOutOfLineTruncateSlow(this);
  }
  FloatRegister src() const { return src_; }
  Register dest() const { return dest_; }
  bool widenFloatToDouble() const { return widenFloatToDouble_; }
  bool preserveTls() const { return preserveTls_; }
  wasm::BytecodeOffset bytecodeOffset() const { return bytecodeOffset_; }
};

OutOfLineCode* CodeGeneratorShared::oolTruncateDouble(
    FloatRegister src, Register dest, MInstruction* mir,
    wasm::BytecodeOffset bytecodeOffset, bool preserveTls) {
  MOZ_ASSERT_IF(IsCompilingWasm(), bytecodeOffset.isValid());

  OutOfLineTruncateSlow* ool = new (alloc()) OutOfLineTruncateSlow(
      src, dest, /* float32 */ false, bytecodeOffset, preserveTls);
  addOutOfLineCode(ool, mir);
  return ool;
}

void CodeGeneratorShared::emitTruncateDouble(FloatRegister src, Register dest,
                                             MInstruction* mir) {
  MOZ_ASSERT(mir->isTruncateToInt32() || mir->isWasmBuiltinTruncateToInt32());
  wasm::BytecodeOffset bytecodeOffset =
      mir->isTruncateToInt32()
          ? mir->toTruncateToInt32()->bytecodeOffset()
          : mir->toWasmBuiltinTruncateToInt32()->bytecodeOffset();
  OutOfLineCode* ool = oolTruncateDouble(src, dest, mir, bytecodeOffset);

  masm.branchTruncateDoubleMaybeModUint32(src, dest, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGeneratorShared::emitTruncateFloat32(FloatRegister src, Register dest,
                                              MInstruction* mir) {
  MOZ_ASSERT(mir->isTruncateToInt32() || mir->isWasmBuiltinTruncateToInt32());
  wasm::BytecodeOffset bytecodeOffset =
      mir->isTruncateToInt32()
          ? mir->toTruncateToInt32()->bytecodeOffset()
          : mir->toWasmBuiltinTruncateToInt32()->bytecodeOffset();
  OutOfLineTruncateSlow* ool = new (alloc())
      OutOfLineTruncateSlow(src, dest, /* float32 */ true, bytecodeOffset);
  addOutOfLineCode(ool, mir);

  masm.branchTruncateFloat32MaybeModUint32(src, dest, ool->entry());
  masm.bind(ool->rejoin());
}

void CodeGeneratorShared::visitOutOfLineTruncateSlow(
    OutOfLineTruncateSlow* ool) {
  FloatRegister src = ool->src();
  Register dest = ool->dest();

  saveVolatile(dest);
  masm.outOfLineTruncateSlow(src, dest, ool->widenFloatToDouble(),
                             gen->compilingWasm(), ool->bytecodeOffset());
  restoreVolatile(dest);

  masm.jump(ool->rejoin());
}

bool CodeGeneratorShared::omitOverRecursedCheck() const {
  // If the current function makes no calls (which means it isn't recursive)
  // and it uses only a small amount of stack space, it doesn't need a
  // stack overflow check. Note that the actual number here is somewhat
  // arbitrary, and codegen actually uses small bounded amounts of
  // additional stack space in some cases too.
  return frameSize() < MAX_UNCHECKED_LEAF_FRAME_SIZE &&
         !gen->needsOverrecursedCheck();
}

void CodeGeneratorShared::emitPreBarrier(Register elements,
                                         const LAllocation* index) {
  if (index->isConstant()) {
    Address address(elements, ToInt32(index) * sizeof(Value));
    masm.guardedCallPreBarrier(address, MIRType::Value);
  } else {
    BaseObjectElementIndex address(elements, ToRegister(index));
    masm.guardedCallPreBarrier(address, MIRType::Value);
  }
}

void CodeGeneratorShared::emitPreBarrier(Address address) {
  masm.guardedCallPreBarrier(address, MIRType::Value);
}

void CodeGeneratorShared::jumpToBlock(MBasicBlock* mir) {
  // Skip past trivial blocks.
  mir = skipTrivialBlocks(mir);

  // No jump necessary if we can fall through to the next block.
  if (isNextBlock(mir->lir())) {
    return;
  }

  masm.jump(mir->lir()->label());
}

Label* CodeGeneratorShared::getJumpLabelForBranch(MBasicBlock* block) {
  // Skip past trivial blocks.
  return skipTrivialBlocks(block)->lir()->label();
}

// This function is not used for MIPS/MIPS64. MIPS has branchToBlock.
#if !defined(JS_CODEGEN_MIPS32) && !defined(JS_CODEGEN_MIPS64)
void CodeGeneratorShared::jumpToBlock(MBasicBlock* mir,
                                      Assembler::Condition cond) {
  // Skip past trivial blocks.
  masm.j(cond, skipTrivialBlocks(mir)->lir()->label());
}
#endif

ReciprocalMulConstants CodeGeneratorShared::computeDivisionConstants(
    uint32_t d, int maxLog) {
  MOZ_ASSERT(maxLog >= 2 && maxLog <= 32);
  // In what follows, 0 < d < 2^maxLog and d is not a power of 2.
  MOZ_ASSERT(d < (uint64_t(1) << maxLog) && (d & (d - 1)) != 0);

  // Speeding up division by non power-of-2 constants is possible by
  // calculating, during compilation, a value M such that high-order
  // bits of M*n correspond to the result of the division of n by d.
  // No value of M can serve this purpose for arbitrarily big values
  // of n but, for optimizing integer division, we're just concerned
  // with values of n whose absolute value is bounded (by fitting in
  // an integer type, say). With this in mind, we'll find a constant
  // M as above that works for -2^maxLog <= n < 2^maxLog; maxLog can
  // then be 31 for signed division or 32 for unsigned division.
  //
  // The original presentation of this technique appears in Hacker's
  // Delight, a book by Henry S. Warren, Jr.. A proof of correctness
  // for our version follows; we'll denote maxLog by L in the proof,
  // for conciseness.
  //
  // Formally, for |d| < 2^L, we'll compute two magic values M and s
  // in the ranges 0 <= M < 2^(L+1) and 0 <= s <= L such that
  //     (M * n) >> (32 + s) = floor(n/d)    if    0 <= n < 2^L
  //     (M * n) >> (32 + s) = ceil(n/d) - 1 if -2^L <= n < 0.
  //
  // Define p = 32 + s, M = ceil(2^p/d), and assume that s satisfies
  //                     M - 2^p/d <= 2^(p-L)/d.                 (1)
  // (Observe that p = CeilLog32(d) + L satisfies this, as the right
  // side of (1) is at least one in this case). Then,
  //
  // a) If p <= CeilLog32(d) + L, then M < 2^(L+1) - 1.
  // Proof: Indeed, M is monotone in p and, for p equal to the above
  // value, the bounds 2^L > d >= 2^(p-L-1) + 1 readily imply that
  //    2^p / d <  2^p/(d - 1) * (d - 1)/d
  //            <= 2^(L+1) * (1 - 1/d) < 2^(L+1) - 2.
  // The claim follows by applying the ceiling function.
  //
  // b) For any 0 <= n < 2^L, floor(Mn/2^p) = floor(n/d).
  // Proof: Put x = floor(Mn/2^p); it's the unique integer for which
  //                    Mn/2^p - 1 < x <= Mn/2^p.                (2)
  // Using M >= 2^p/d on the LHS and (1) on the RHS, we get
  //           n/d - 1 < x <= n/d + n/(2^L d) < n/d + 1/d.
  // Since x is an integer, it's not in the interval (n/d, (n+1)/d),
  // and so n/d - 1 < x <= n/d, which implies x = floor(n/d).
  //
  // c) For any -2^L <= n < 0, floor(Mn/2^p) + 1 = ceil(n/d).
  // Proof: The proof is similar. Equation (2) holds as above. Using
  // M > 2^p/d (d isn't a power of 2) on the RHS and (1) on the LHS,
  //                 n/d + n/(2^L d) - 1 < x < n/d.
  // Using n >= -2^L and summing 1,
  //                  n/d - 1/d < x + 1 < n/d + 1.
  // Since x + 1 is an integer, this implies n/d <= x + 1 < n/d + 1.
  // In other words, x + 1 = ceil(n/d).
  //
  // Condition (1) isn't necessary for the existence of M and s with
  // the properties above. Hacker's Delight provides a slightly less
  // restrictive condition when d >= 196611, at the cost of a 3-page
  // proof of correctness, for the case L = 31.
  //
  // Note that, since d*M - 2^p = d - (2^p)%d, (1) can be written as
  //                   2^(p-L) >= d - (2^p)%d.
  // In order to avoid overflow in the (2^p) % d calculation, we can
  // compute it as (2^p-1) % d + 1, where 2^p-1 can then be computed
  // without overflow as UINT64_MAX >> (64-p).

  // We now compute the least p >= 32 with the property above...
  int32_t p = 32;
  while ((uint64_t(1) << (p - maxLog)) + (UINT64_MAX >> (64 - p)) % d + 1 < d) {
    p++;
  }

  // ...and the corresponding M. For either the signed (L=31) or the
  // unsigned (L=32) case, this value can be too large (cf. item a).
  // Codegen can still multiply by M by multiplying by (M - 2^L) and
  // adjusting the value afterwards, if this is the case.
  ReciprocalMulConstants rmc;
  rmc.multiplier = (UINT64_MAX >> (64 - p)) / d + 1;
  rmc.shiftAmount = p - 32;

  return rmc;
}

#ifdef JS_TRACE_LOGGING

void CodeGeneratorShared::emitTracelogScript(bool isStart) {
  if (!TraceLogTextIdEnabled(TraceLogger_Scripts)) {
    return;
  }

  Label done;

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  Register logger = regs.takeAnyGeneral();
  Register script = regs.takeAnyGeneral();

  masm.Push(logger);

  masm.loadTraceLogger(logger);
  masm.branchTestPtr(Assembler::Zero, logger, logger, &done);

  Address enabledAddress(logger, TraceLoggerThread::offsetOfEnabled());
  masm.branch32(Assembler::Equal, enabledAddress, Imm32(0), &done);

  masm.Push(script);

  CodeOffset patchScript = masm.movWithPatch(ImmWord(0), script);
  masm.propagateOOM(patchableTLScripts_.append(patchScript));

  if (isStart) {
    masm.tracelogStartId(logger, script);
  } else {
    masm.tracelogStopId(logger, script);
  }

  masm.Pop(script);

  masm.bind(&done);

  masm.Pop(logger);
}

void CodeGeneratorShared::emitTracelogTree(bool isStart, uint32_t textId) {
  if (!TraceLogTextIdEnabled(textId)) {
    return;
  }

  Label done;
  AllocatableRegisterSet regs(RegisterSet::Volatile());
  Register logger = regs.takeAnyGeneral();

  masm.Push(logger);

  masm.loadTraceLogger(logger);
  masm.branchTestPtr(Assembler::Zero, logger, logger, &done);

  Address enabledAddress(logger, TraceLoggerThread::offsetOfEnabled());
  masm.branch32(Assembler::Equal, enabledAddress, Imm32(0), &done);

  if (isStart) {
    masm.tracelogStartId(logger, textId);
  } else {
    masm.tracelogStopId(logger, textId);
  }

  masm.bind(&done);

  masm.Pop(logger);
}

void CodeGeneratorShared::emitTracelogTree(bool isStart, const char* text,
                                           TraceLoggerTextId enabledTextId) {
  if (!TraceLogTextIdEnabled(enabledTextId)) {
    return;
  }

  Label done;

  AllocatableRegisterSet regs(RegisterSet::Volatile());
  Register loggerReg = regs.takeAnyGeneral();
  Register eventReg = regs.takeAnyGeneral();

  masm.Push(loggerReg);

  masm.loadTraceLogger(loggerReg);
  masm.branchTestPtr(Assembler::Zero, loggerReg, loggerReg, &done);

  Address enabledAddress(loggerReg, TraceLoggerThread::offsetOfEnabled());
  masm.branch32(Assembler::Equal, enabledAddress, Imm32(0), &done);

  masm.Push(eventReg);

  PatchableTLEvent patchEvent(masm.movWithPatch(ImmWord(0), eventReg), text);
  masm.propagateOOM(patchableTLEvents_.append(std::move(patchEvent)));

  if (isStart) {
    masm.tracelogStartId(loggerReg, eventReg);
  } else {
    masm.tracelogStopId(loggerReg, eventReg);
  }

  masm.Pop(eventReg);

  masm.bind(&done);

  masm.Pop(loggerReg);
}
#endif

}  // namespace jit
}  // namespace js
