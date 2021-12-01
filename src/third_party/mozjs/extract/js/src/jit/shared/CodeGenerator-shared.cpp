/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/shared/CodeGenerator-shared-inl.h"

#include "mozilla/DebugOnly.h"

#include "jit/CompactBuffer.h"
#include "jit/JitcodeMap.h"
#include "jit/JitSpewer.h"
#include "jit/MacroAssembler.h"
#include "jit/MIR.h"
#include "jit/MIRGenerator.h"
#include "jit/OptimizationTracking.h"
#include "js/Conversions.h"
#include "vm/TraceLogging.h"

#include "jit/JitFrames-inl.h"
#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;

using mozilla::BitwiseCast;
using mozilla::DebugOnly;

namespace js {
namespace jit {

MacroAssembler&
CodeGeneratorShared::ensureMasm(MacroAssembler* masmArg)
{
    if (masmArg)
        return *masmArg;
    maybeMasm_.emplace();
    return *maybeMasm_;
}

CodeGeneratorShared::CodeGeneratorShared(MIRGenerator* gen, LIRGraph* graph, MacroAssembler* masmArg)
  : maybeMasm_(),
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
    safepoints_(graph->totalSlotCount(), (gen->info().nargs() + 1) * sizeof(Value)),
    returnLabel_(),
    stubSpace_(),
    nativeToBytecodeMap_(nullptr),
    nativeToBytecodeMapSize_(0),
    nativeToBytecodeTableOffset_(0),
    nativeToBytecodeNumRegions_(0),
    nativeToBytecodeScriptList_(nullptr),
    nativeToBytecodeScriptListLength_(0),
    trackedOptimizationsMap_(nullptr),
    trackedOptimizationsMapSize_(0),
    trackedOptimizationsRegionTableOffset_(0),
    trackedOptimizationsTypesTableOffset_(0),
    trackedOptimizationsAttemptsTableOffset_(0),
    osrEntryOffset_(0),
    skipArgCheckEntryOffset_(0),
#ifdef CHECK_OSIPOINT_REGISTERS
    checkOsiPointRegisters(JitOptions.checkOsiPointRegisters),
#endif
    frameDepth_(graph->paddedLocalSlotsSize() + graph->argumentsSize()),
    frameInitialAdjustment_(0)
{
    if (gen->isProfilerInstrumentationEnabled())
        masm.enableProfilingInstrumentation();

    if (gen->compilingWasm()) {
        // Since wasm uses the system ABI which does not necessarily use a
        // regular array where all slots are sizeof(Value), it maintains the max
        // argument stack depth separately.
        MOZ_ASSERT(graph->argumentSlotCount() == 0);
        frameDepth_ += gen->wasmMaxStackArgBytes();

        if (gen->usesSimd()) {
            // If the function uses any SIMD then we may need to insert padding
            // so that local slots are aligned for SIMD.
            frameInitialAdjustment_ = ComputeByteAlignment(sizeof(wasm::Frame), WasmStackAlignment);
            frameDepth_ += frameInitialAdjustment_;

            // Keep the stack aligned. Some SIMD sequences build values on the
            // stack and need the stack aligned.
            frameDepth_ += ComputeByteAlignment(sizeof(wasm::Frame) + frameDepth_,
                                                WasmStackAlignment);
        } else if (gen->needsStaticStackAlignment()) {
            // An MWasmCall does not align the stack pointer at calls sites but
            // instead relies on the a priori stack adjustment. This must be the
            // last adjustment of frameDepth_.
            frameDepth_ += ComputeByteAlignment(sizeof(wasm::Frame) + frameDepth_,
                                                WasmStackAlignment);
        }

        // FrameSizeClass is only used for bailing, which cannot happen in
        // wasm code.
        frameClass_ = FrameSizeClass::None();
    } else {
        frameClass_ = FrameSizeClass::FromDepth(frameDepth_);
    }
}

bool
CodeGeneratorShared::generatePrologue()
{
    MOZ_ASSERT(masm.framePushed() == 0);
    MOZ_ASSERT(!gen->compilingWasm());

#ifdef JS_USE_LINK_REGISTER
    masm.pushReturnAddress();
#endif

    // If profiling, save the current frame pointer to a per-thread global field.
    if (isProfilerInstrumentationEnabled())
        masm.profilerEnterFrame(masm.getStackPointer(), CallTempReg0);

    // Ensure that the Ion frame is properly aligned.
    masm.assertStackAlignment(JitStackAlignment, 0);

    // Note that this automatically sets MacroAssembler::framePushed().
    masm.reserveStack(frameSize());
    masm.checkStackAlignment();

    emitTracelogIonStart();
    return true;
}

bool
CodeGeneratorShared::generateEpilogue()
{
    MOZ_ASSERT(!gen->compilingWasm());
    masm.bind(&returnLabel_);

    emitTracelogIonStop();

    masm.freeStack(frameSize());
    MOZ_ASSERT(masm.framePushed() == 0);

    // If profiling, reset the per-thread global lastJitFrame to point to
    // the previous frame.
    if (isProfilerInstrumentationEnabled())
        masm.profilerExitFrame();

    masm.ret();

    // On systems that use a constant pool, this is a good time to emit.
    masm.flushBuffer();
    return true;
}

bool
CodeGeneratorShared::generateOutOfLineCode()
{
    // OOL paths should not attempt to use |current| as it's the last block
    // instead of the block corresponding to the OOL path.
    current = nullptr;

    for (size_t i = 0; i < outOfLineCode_.length(); i++) {
        // Add native => bytecode mapping entries for OOL sites.
        // Not enabled on wasm yet since it doesn't contain bytecode mappings.
        if (!gen->compilingWasm()) {
            if (!addNativeToBytecodeEntry(outOfLineCode_[i]->bytecodeSite()))
                return false;
        }

        if (!gen->alloc().ensureBallast())
            return false;

        JitSpew(JitSpew_Codegen, "# Emitting out of line code");

        masm.setFramePushed(outOfLineCode_[i]->framePushed());
        lastPC_ = outOfLineCode_[i]->pc();
        outOfLineCode_[i]->bind(&masm);

        outOfLineCode_[i]->generate(this);
    }

    return !masm.oom();
}

void
CodeGeneratorShared::addOutOfLineCode(OutOfLineCode* code, const MInstruction* mir)
{
    MOZ_ASSERT(mir);
    addOutOfLineCode(code, mir->trackedSite());
}

void
CodeGeneratorShared::addOutOfLineCode(OutOfLineCode* code, const BytecodeSite* site)
{
    code->setFramePushed(masm.framePushed());
    code->setBytecodeSite(site);
    MOZ_ASSERT_IF(!gen->compilingWasm(), code->script()->containsPC(code->pc()));
    masm.propagateOOM(outOfLineCode_.append(code));
}

bool
CodeGeneratorShared::addNativeToBytecodeEntry(const BytecodeSite* site)
{
    // Skip the table entirely if profiling is not enabled.
    if (!isProfilerInstrumentationEnabled())
        return true;

    // Fails early if the last added instruction caused the macro assembler to
    // run out of memory as continuity assumption below do not hold.
    if (masm.oom())
        return false;

    MOZ_ASSERT(site);
    MOZ_ASSERT(site->tree());
    MOZ_ASSERT(site->pc());

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
                if (nextToLastEntry.tree == lastEntry.tree && nextToLastEntry.pc == lastEntry.pc) {
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
    if (!nativeToBytecodeList_.append(entry))
        return false;

    JitSpew(JitSpew_Profiling, " => Push new entry.");
    dumpNativeToBytecodeEntry(nativeToBytecodeList_.length() - 1);
    return true;
}

void
CodeGeneratorShared::dumpNativeToBytecodeEntries()
{
#ifdef JS_JITSPEW
    InlineScriptTree* topTree = gen->info().inlineScriptTree();
    JitSpewStart(JitSpew_Profiling, "Native To Bytecode Entries for %s:%zu\n",
                 topTree->script()->filename(), topTree->script()->lineno());
    for (unsigned i = 0; i < nativeToBytecodeList_.length(); i++)
        dumpNativeToBytecodeEntry(i);
#endif
}

void
CodeGeneratorShared::dumpNativeToBytecodeEntry(uint32_t idx)
{
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
        if (nextRef->tree == ref.tree)
            pcDelta = nextRef->pc - ref.pc;
    }
    JitSpewStart(JitSpew_Profiling, "    %08zx [+%-6d] => %-6ld [%-4d] {%-10s} (%s:%zu",
                 ref.nativeOffset.offset(),
                 nativeDelta,
                 (long) (ref.pc - script->code()),
                 pcDelta,
                 CodeName[JSOp(*ref.pc)],
                 script->filename(), script->lineno());

    for (tree = tree->caller(); tree; tree = tree->caller()) {
        JitSpewCont(JitSpew_Profiling, " <= %s:%zu", tree->script()->filename(),
                                                    tree->script()->lineno());
    }
    JitSpewCont(JitSpew_Profiling, ")");
    JitSpewFin(JitSpew_Profiling);
#endif
}

bool
CodeGeneratorShared::addTrackedOptimizationsEntry(const TrackedOptimizations* optimizations)
{
    if (!isOptimizationTrackingEnabled())
        return true;

    MOZ_ASSERT(optimizations);

    uint32_t nativeOffset = masm.currentOffset();

    if (!trackedOptimizations_.empty()) {
        NativeToTrackedOptimizations& lastEntry = trackedOptimizations_.back();
        MOZ_ASSERT_IF(!masm.oom(), nativeOffset >= lastEntry.endOffset.offset());

        // If we're still generating code for the same set of optimizations,
        // we are done.
        if (lastEntry.optimizations == optimizations)
            return true;
    }

    // If we're generating code for a new set of optimizations, add a new
    // entry.
    NativeToTrackedOptimizations entry;
    entry.startOffset = CodeOffset(nativeOffset);
    entry.endOffset = CodeOffset(nativeOffset);
    entry.optimizations = optimizations;
    return trackedOptimizations_.append(entry);
}

void
CodeGeneratorShared::extendTrackedOptimizationsEntry(const TrackedOptimizations* optimizations)
{
    if (!isOptimizationTrackingEnabled())
        return;

    uint32_t nativeOffset = masm.currentOffset();
    NativeToTrackedOptimizations& entry = trackedOptimizations_.back();
    MOZ_ASSERT(entry.optimizations == optimizations);
    MOZ_ASSERT_IF(!masm.oom(), nativeOffset >= entry.endOffset.offset());

    entry.endOffset = CodeOffset(nativeOffset);

    // If we generated no code, remove the last entry.
    if (nativeOffset == entry.startOffset.offset())
        trackedOptimizations_.popBack();
}

// see OffsetOfFrameSlot
static inline int32_t
ToStackIndex(LAllocation* a)
{
    if (a->isStackSlot()) {
        MOZ_ASSERT(a->toStackSlot()->slot() >= 1);
        return a->toStackSlot()->slot();
    }
    return -int32_t(sizeof(JitFrameLayout) + a->toArgument()->index());
}

void
CodeGeneratorShared::encodeAllocation(LSnapshot* snapshot, MDefinition* mir,
                                      uint32_t* allocIndex)
{
    if (mir->isBox())
        mir = mir->toBox()->getOperand(0);

    MIRType type =
        mir->isRecoveredOnBailout() ? MIRType::None :
        mir->isUnused() ? MIRType::MagicOptimizedOut :
        mir->type();

    RValueAllocation alloc;

    switch (type) {
      case MIRType::None:
      {
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
        if (mir->isLambda() || mir->isLambdaArrow()) {
            MConstant* constant = mir->isLambda() ? mir->toLambda()->functionOperand()
                                                  : mir->toLambdaArrow()->functionOperand();
            uint32_t cstIndex;
            masm.propagateOOM(graph.addConstantToPool(constant->toJSValue(), &cstIndex));
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
      case MIRType::Object:
      case MIRType::ObjectOrNull:
      case MIRType::Boolean:
      case MIRType::Double:
      {
        LAllocation* payload = snapshot->payloadOfSlot(*allocIndex);
        if (payload->isConstant()) {
            MConstant* constant = mir->toConstant();
            uint32_t index;
            masm.propagateOOM(graph.addConstantToPool(constant->toJSValue(), &index));
            alloc = RValueAllocation::ConstantPool(index);
            break;
        }

        JSValueType valueType =
            (type == MIRType::ObjectOrNull) ? JSVAL_TYPE_OBJECT : ValueTypeFromMIRType(type);

        MOZ_DIAGNOSTIC_ASSERT(payload->isMemory() || payload->isRegister());
        if (payload->isMemory())
            alloc = RValueAllocation::Typed(valueType, ToStackIndex(payload));
        else if (payload->isGeneralReg())
            alloc = RValueAllocation::Typed(valueType, ToRegister(payload));
        else if (payload->isFloatReg())
            alloc = RValueAllocation::Double(ToFloatRegister(payload));
        else
            MOZ_CRASH("Unexpected payload type.");
        break;
      }
      case MIRType::Float32:
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Float32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
      {
        LAllocation* payload = snapshot->payloadOfSlot(*allocIndex);
        if (payload->isConstant()) {
            MConstant* constant = mir->toConstant();
            uint32_t index;
            masm.propagateOOM(graph.addConstantToPool(constant->toJSValue(), &index));
            alloc = RValueAllocation::ConstantPool(index);
            break;
        }

        MOZ_ASSERT(payload->isMemory() || payload->isFloatReg());
        if (payload->isFloatReg())
            alloc = RValueAllocation::AnyFloat(ToFloatRegister(payload));
        else
            alloc = RValueAllocation::AnyFloat(ToStackIndex(payload));
        break;
      }
      case MIRType::MagicOptimizedArguments:
      case MIRType::MagicOptimizedOut:
      case MIRType::MagicUninitializedLexical:
      case MIRType::MagicIsConstructing:
      {
        uint32_t index;
        JSWhyMagic why = JS_GENERIC_MAGIC;
        switch (type) {
          case MIRType::MagicOptimizedArguments:
            why = JS_OPTIMIZED_ARGUMENTS;
            break;
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
      default:
      {
        MOZ_ASSERT(mir->type() == MIRType::Value);
        LAllocation* payload = snapshot->payloadOfSlot(*allocIndex);
#ifdef JS_NUNBOX32
        LAllocation* type = snapshot->typeOfSlot(*allocIndex);
        if (type->isRegister()) {
            if (payload->isRegister())
                alloc = RValueAllocation::Untyped(ToRegister(type), ToRegister(payload));
            else
                alloc = RValueAllocation::Untyped(ToRegister(type), ToStackIndex(payload));
        } else {
            if (payload->isRegister())
                alloc = RValueAllocation::Untyped(ToStackIndex(type), ToRegister(payload));
            else
                alloc = RValueAllocation::Untyped(ToStackIndex(type), ToStackIndex(payload));
        }
#elif JS_PUNBOX64
        if (payload->isRegister())
            alloc = RValueAllocation::Untyped(ToRegister(payload));
        else
            alloc = RValueAllocation::Untyped(ToStackIndex(payload));
#endif
        break;
      }
    }
    MOZ_DIAGNOSTIC_ASSERT(alloc.valid());

    // This set an extra bit as part of the RValueAllocation, such that we know
    // that recover instruction have to be executed without wrapping the
    // instruction in a no-op recover instruction.
    if (mir->isIncompleteObject())
        alloc.setNeedSideEffect();

    masm.propagateOOM(snapshots_.add(alloc));

    *allocIndex += mir->isRecoveredOnBailout() ? 0 : 1;
}

void
CodeGeneratorShared::encode(LRecoverInfo* recover)
{
    if (recover->recoverOffset() != INVALID_RECOVER_OFFSET)
        return;

    uint32_t numInstructions = recover->numInstructions();
    JitSpew(JitSpew_IonSnapshots, "Encoding LRecoverInfo %p (frameCount %u, instructions %u)",
            (void*)recover, recover->mir()->frameCount(), numInstructions);

    MResumePoint::Mode mode = recover->mir()->mode();
    MOZ_ASSERT(mode != MResumePoint::Outer);
    bool resumeAfter = (mode == MResumePoint::ResumeAfter);

    RecoverOffset offset = recovers_.startRecover(numInstructions, resumeAfter);

    for (MNode* insn : *recover)
        recovers_.writeInstruction(insn);

    recovers_.endRecover();
    recover->setRecoverOffset(offset);
    masm.propagateOOM(!recovers_.oom());
}

void
CodeGeneratorShared::encode(LSnapshot* snapshot)
{
    if (snapshot->snapshotOffset() != INVALID_SNAPSHOT_OFFSET)
        return;

    LRecoverInfo* recoverInfo = snapshot->recoverInfo();
    encode(recoverInfo);

    RecoverOffset recoverOffset = recoverInfo->recoverOffset();
    MOZ_ASSERT(recoverOffset != INVALID_RECOVER_OFFSET);

    JitSpew(JitSpew_IonSnapshots, "Encoding LSnapshot %p (LRecover %p)",
            (void*)snapshot, (void*) recoverInfo);

    SnapshotOffset offset = snapshots_.startSnapshot(recoverOffset, snapshot->bailoutKind());

#ifdef TRACK_SNAPSHOTS
    uint32_t pcOpcode = 0;
    uint32_t lirOpcode = 0;
    uint32_t lirId = 0;
    uint32_t mirOpcode = 0;
    uint32_t mirId = 0;

    if (LNode* ins = instruction()) {
        lirOpcode = ins->op();
        lirId = ins->id();
        if (ins->mirRaw()) {
            mirOpcode = uint32_t(ins->mirRaw()->op());
            mirId = ins->mirRaw()->id();
            if (ins->mirRaw()->trackedPc())
                pcOpcode = *ins->mirRaw()->trackedPc();
        }
    }
    snapshots_.trackSnapshot(pcOpcode, mirOpcode, mirId, lirOpcode, lirId);
#endif

    uint32_t allocIndex = 0;
    for (LRecoverInfo::OperandIter it(recoverInfo); !it; ++it) {
        DebugOnly<uint32_t> allocWritten = snapshots_.allocWritten();
        encodeAllocation(snapshot, *it, &allocIndex);
        MOZ_ASSERT_IF(!snapshots_.oom(), allocWritten + 1 == snapshots_.allocWritten());
    }

    MOZ_ASSERT(allocIndex == snapshot->numSlots());
    snapshots_.endSnapshot();
    snapshot->setSnapshotOffset(offset);
    masm.propagateOOM(!snapshots_.oom());
}

bool
CodeGeneratorShared::assignBailoutId(LSnapshot* snapshot)
{
    MOZ_ASSERT(snapshot->snapshotOffset() != INVALID_SNAPSHOT_OFFSET);

    // Can we not use bailout tables at all?
    if (!deoptTable_)
        return false;

    MOZ_ASSERT(frameClass_ != FrameSizeClass::None());

    if (snapshot->bailoutId() != INVALID_BAILOUT_ID)
        return true;

    // Is the bailout table full?
    if (bailouts_.length() >= BAILOUT_TABLE_SIZE)
        return false;

    unsigned bailoutId = bailouts_.length();
    snapshot->setBailoutId(bailoutId);
    JitSpew(JitSpew_IonSnapshots, "Assigned snapshot bailout id %u", bailoutId);
    masm.propagateOOM(bailouts_.append(snapshot->snapshotOffset()));
    return true;
}

bool
CodeGeneratorShared::encodeSafepoints()
{
    for (SafepointIndex& index : safepointIndices_) {
        LSafepoint* safepoint = index.safepoint();

        if (!safepoint->encoded())
            safepoints_.encode(safepoint);

        index.resolve();
    }

    return !safepoints_.oom();
}

bool
CodeGeneratorShared::createNativeToBytecodeScriptList(JSContext* cx)
{
    js::Vector<JSScript*, 0, SystemAllocPolicy> scriptList;
    InlineScriptTree* tree = gen->info().inlineScriptTree();
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
            if (!scriptList.append(tree->script()))
                return false;
        }

        // Process rest of tree

        // If children exist, emit children.
        if (tree->hasChildren()) {
            tree = tree->firstChild();
            continue;
        }

        // Otherwise, find the first tree up the chain (including this one)
        // that contains a next sibling.
        while (!tree->hasNextCallee() && tree->hasCaller())
            tree = tree->caller();

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
    JSScript** data = cx->zone()->pod_malloc<JSScript*>(scriptList.length());
    if (!data)
        return false;

    for (uint32_t i = 0; i < scriptList.length(); i++)
        data[i] = scriptList[i];

    // Success.
    nativeToBytecodeScriptListLength_ = scriptList.length();
    nativeToBytecodeScriptList_ = data;
    return true;
}

bool
CodeGeneratorShared::generateCompactNativeToBytecodeMap(JSContext* cx, JitCode* code)
{
    MOZ_ASSERT(nativeToBytecodeScriptListLength_ == 0);
    MOZ_ASSERT(nativeToBytecodeScriptList_ == nullptr);
    MOZ_ASSERT(nativeToBytecodeMap_ == nullptr);
    MOZ_ASSERT(nativeToBytecodeMapSize_ == 0);
    MOZ_ASSERT(nativeToBytecodeTableOffset_ == 0);
    MOZ_ASSERT(nativeToBytecodeNumRegions_ == 0);

    if (!createNativeToBytecodeScriptList(cx))
        return false;

    MOZ_ASSERT(nativeToBytecodeScriptListLength_ > 0);
    MOZ_ASSERT(nativeToBytecodeScriptList_ != nullptr);

    CompactBufferWriter writer;
    uint32_t tableOffset = 0;
    uint32_t numRegions = 0;

    if (!JitcodeIonTable::WriteIonTable(
            writer, nativeToBytecodeScriptList_, nativeToBytecodeScriptListLength_,
            &nativeToBytecodeList_[0],
            &nativeToBytecodeList_[0] + nativeToBytecodeList_.length(),
            &tableOffset, &numRegions))
    {
        js_free(nativeToBytecodeScriptList_);
        return false;
    }

    MOZ_ASSERT(tableOffset > 0);
    MOZ_ASSERT(numRegions > 0);

    // Writer is done, copy it to sized buffer.
    uint8_t* data = cx->zone()->pod_malloc<uint8_t>(writer.length());
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

    JitSpew(JitSpew_Profiling, "Compact Native To Bytecode Map [%p-%p]",
            data, data + nativeToBytecodeMapSize_);

    return true;
}

void
CodeGeneratorShared::verifyCompactNativeToBytecodeMap(JitCode* code)
{
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
    const JitcodeIonTable* ionTable = reinterpret_cast<const JitcodeIonTable*>(tablePtr);
    MOZ_ASSERT(ionTable->numRegions() == nativeToBytecodeNumRegions_);

    // Region offset for first region should be at the start of the payload region.
    // Since the offsets are backward from the start of the table, the first entry
    // backoffset should be equal to the forward table offset from the start of the
    // allocated data.
    MOZ_ASSERT(ionTable->regionOffset(0) == nativeToBytecodeTableOffset_);

    // Verify each region.
    for (uint32_t i = 0; i < ionTable->numRegions(); i++) {
        // Back-offset must point into the payload region preceding the table, not before it.
        MOZ_ASSERT(ionTable->regionOffset(i) <= nativeToBytecodeTableOffset_);

        // Back-offset must point to a later area in the payload region than previous
        // back-offset.  This means that back-offsets decrease monotonically.
        MOZ_ASSERT_IF(i > 0, ionTable->regionOffset(i) < ionTable->regionOffset(i - 1));

        JitcodeRegionEntry entry = ionTable->regionEntry(i);

        // Ensure native code offset for region falls within jitcode.
        MOZ_ASSERT(entry.nativeOffset() <= code->instructionsSize());

        // Read out script/pc stack and verify.
        JitcodeRegionEntry::ScriptPcIterator scriptPcIter = entry.scriptPcIterator();
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
#endif // DEBUG
}

bool
CodeGeneratorShared::generateCompactTrackedOptimizationsMap(JSContext* cx, JitCode* code,
                                                            IonTrackedTypeVector* allTypes)
{
    MOZ_ASSERT(trackedOptimizationsMap_ == nullptr);
    MOZ_ASSERT(trackedOptimizationsMapSize_ == 0);
    MOZ_ASSERT(trackedOptimizationsRegionTableOffset_ == 0);
    MOZ_ASSERT(trackedOptimizationsTypesTableOffset_ == 0);
    MOZ_ASSERT(trackedOptimizationsAttemptsTableOffset_ == 0);

    if (trackedOptimizations_.empty())
        return true;

    UniqueTrackedOptimizations unique(cx);
    if (!unique.init())
        return false;

    // Iterate through all entries to deduplicate their optimization attempts.
    for (size_t i = 0; i < trackedOptimizations_.length(); i++) {
        NativeToTrackedOptimizations& entry = trackedOptimizations_[i];
        if (!unique.add(entry.optimizations))
            return false;
    }

    // Sort the unique optimization attempts by frequency to stabilize the
    // attempts' indices in the compact table we will write later.
    if (!unique.sortByFrequency(cx))
        return false;

    // Write out the ranges and the table.
    CompactBufferWriter writer;
    uint32_t numRegions;
    uint32_t regionTableOffset;
    uint32_t typesTableOffset;
    uint32_t attemptsTableOffset;
    if (!WriteIonTrackedOptimizationsTable(cx, writer,
                                           trackedOptimizations_.begin(),
                                           trackedOptimizations_.end(),
                                           unique, &numRegions,
                                           &regionTableOffset, &typesTableOffset,
                                           &attemptsTableOffset, allTypes))
    {
        return false;
    }

    MOZ_ASSERT(regionTableOffset > 0);
    MOZ_ASSERT(typesTableOffset > 0);
    MOZ_ASSERT(attemptsTableOffset > 0);
    MOZ_ASSERT(typesTableOffset > regionTableOffset);
    MOZ_ASSERT(attemptsTableOffset > typesTableOffset);

    // Copy over the table out of the writer's buffer.
    uint8_t* data = cx->zone()->pod_malloc<uint8_t>(writer.length());
    if (!data)
        return false;

    memcpy(data, writer.buffer(), writer.length());
    trackedOptimizationsMap_ = data;
    trackedOptimizationsMapSize_ = writer.length();
    trackedOptimizationsRegionTableOffset_ = regionTableOffset;
    trackedOptimizationsTypesTableOffset_ = typesTableOffset;
    trackedOptimizationsAttemptsTableOffset_ = attemptsTableOffset;

    verifyCompactTrackedOptimizationsMap(code, numRegions, unique, allTypes);

    JitSpew(JitSpew_OptimizationTrackingExtended,
            "== Compact Native To Optimizations Map [%p-%p] size %u",
            data, data + trackedOptimizationsMapSize_, trackedOptimizationsMapSize_);
    JitSpew(JitSpew_OptimizationTrackingExtended,
            "     with type list of length %zu, size %zu",
            allTypes->length(), allTypes->length() * sizeof(IonTrackedTypeWithAddendum));

    return true;
}

#ifdef DEBUG
class ReadTempAttemptsVectorOp : public JS::ForEachTrackedOptimizationAttemptOp
{
    TempOptimizationAttemptsVector* attempts_;
    bool oom_;

  public:
    explicit ReadTempAttemptsVectorOp(TempOptimizationAttemptsVector* attempts)
      : attempts_(attempts), oom_(false)
    { }

    bool oom() {
        return oom_;
    }

    void operator()(JS::TrackedStrategy strategy, JS::TrackedOutcome outcome) override {
        if (!attempts_->append(OptimizationAttempt(strategy, outcome)))
            oom_ = true;
    }
};

struct ReadTempTypeInfoVectorOp : public IonTrackedOptimizationsTypeInfo::ForEachOp
{
    TempAllocator& alloc_;
    TempOptimizationTypeInfoVector* types_;
    TempTypeList accTypes_;
    bool oom_;

  public:
    ReadTempTypeInfoVectorOp(TempAllocator& alloc, TempOptimizationTypeInfoVector* types)
      : alloc_(alloc),
        types_(types),
        accTypes_(alloc),
        oom_(false)
    { }

    bool oom() {
        return oom_;
    }

    void readType(const IonTrackedTypeWithAddendum& tracked) override {
        if (!accTypes_.append(tracked.type))
            oom_ = true;
    }

    void operator()(JS::TrackedTypeSite site, MIRType mirType) override {
        OptimizationTypeInfo ty(alloc_, site, mirType);
        for (uint32_t i = 0; i < accTypes_.length(); i++) {
            if (!ty.trackType(accTypes_[i]))
                oom_ = true;
        }
        if (!types_->append(mozilla::Move(ty)))
            oom_ = true;
        accTypes_.clear();
    }
};
#endif // DEBUG

void
CodeGeneratorShared::verifyCompactTrackedOptimizationsMap(JitCode* code, uint32_t numRegions,
                                                          const UniqueTrackedOptimizations& unique,
                                                          const IonTrackedTypeVector* allTypes)
{
#ifdef DEBUG
    MOZ_ASSERT(trackedOptimizationsMap_ != nullptr);
    MOZ_ASSERT(trackedOptimizationsMapSize_ > 0);
    MOZ_ASSERT(trackedOptimizationsRegionTableOffset_ > 0);
    MOZ_ASSERT(trackedOptimizationsTypesTableOffset_ > 0);
    MOZ_ASSERT(trackedOptimizationsAttemptsTableOffset_ > 0);

    // Table pointers must all be 4-byte aligned.
    const uint8_t* regionTableAddr = trackedOptimizationsMap_ +
                                     trackedOptimizationsRegionTableOffset_;
    const uint8_t* typesTableAddr = trackedOptimizationsMap_ +
                                    trackedOptimizationsTypesTableOffset_;
    const uint8_t* attemptsTableAddr = trackedOptimizationsMap_ +
                                       trackedOptimizationsAttemptsTableOffset_;
    MOZ_ASSERT(uintptr_t(regionTableAddr) % sizeof(uint32_t) == 0);
    MOZ_ASSERT(uintptr_t(typesTableAddr) % sizeof(uint32_t) == 0);
    MOZ_ASSERT(uintptr_t(attemptsTableAddr) % sizeof(uint32_t) == 0);

    // Assert that the number of entries matches up for the tables.
    const IonTrackedOptimizationsRegionTable* regionTable =
        (const IonTrackedOptimizationsRegionTable*) regionTableAddr;
    MOZ_ASSERT(regionTable->numEntries() == numRegions);
    const IonTrackedOptimizationsTypesTable* typesTable =
        (const IonTrackedOptimizationsTypesTable*) typesTableAddr;
    MOZ_ASSERT(typesTable->numEntries() == unique.count());
    const IonTrackedOptimizationsAttemptsTable* attemptsTable =
        (const IonTrackedOptimizationsAttemptsTable*) attemptsTableAddr;
    MOZ_ASSERT(attemptsTable->numEntries() == unique.count());

    // Verify each region.
    uint32_t trackedIdx = 0;
    for (uint32_t regionIdx = 0; regionIdx < regionTable->numEntries(); regionIdx++) {
        // Check reverse offsets are within bounds.
        MOZ_ASSERT(regionTable->entryOffset(regionIdx) <= trackedOptimizationsRegionTableOffset_);
        MOZ_ASSERT_IF(regionIdx > 0, regionTable->entryOffset(regionIdx) <
                                     regionTable->entryOffset(regionIdx - 1));

        IonTrackedOptimizationsRegion region = regionTable->entry(regionIdx);

        // Check the region range is covered by jitcode.
        MOZ_ASSERT(region.startOffset() <= code->instructionsSize());
        MOZ_ASSERT(region.endOffset() <= code->instructionsSize());

        IonTrackedOptimizationsRegion::RangeIterator iter = region.ranges();
        while (iter.more()) {
            // Assert that the offsets are correctly decoded from the delta.
            uint32_t startOffset, endOffset;
            uint8_t index;
            iter.readNext(&startOffset, &endOffset, &index);
            NativeToTrackedOptimizations& entry = trackedOptimizations_[trackedIdx++];
            MOZ_ASSERT(startOffset == entry.startOffset.offset());
            MOZ_ASSERT(endOffset == entry.endOffset.offset());
            MOZ_ASSERT(index == unique.indexOf(entry.optimizations));

            // Assert that the type info and attempts vectors are correctly
            // decoded. This is disabled for now if the types table might
            // contain nursery pointers, in which case the types might not
            // match, see bug 1175761.
            if (!code->zone()->group()->storeBuffer().cancelIonCompilations()) {
                IonTrackedOptimizationsTypeInfo typeInfo = typesTable->entry(index);
                TempOptimizationTypeInfoVector tvec(alloc());
                ReadTempTypeInfoVectorOp top(alloc(), &tvec);
                typeInfo.forEach(top, allTypes);
                MOZ_ASSERT_IF(!top.oom(), entry.optimizations->matchTypes(tvec));
            }

            IonTrackedOptimizationsAttempts attempts = attemptsTable->entry(index);
            TempOptimizationAttemptsVector avec(alloc());
            ReadTempAttemptsVectorOp aop(&avec);
            attempts.forEach(aop);
            MOZ_ASSERT_IF(!aop.oom(), entry.optimizations->matchAttempts(avec));
        }
    }
#endif
}

void
CodeGeneratorShared::markSafepoint(LInstruction* ins)
{
    markSafepointAt(masm.currentOffset(), ins);
}

void
CodeGeneratorShared::markSafepointAt(uint32_t offset, LInstruction* ins)
{
    MOZ_ASSERT_IF(!safepointIndices_.empty() && !masm.oom(),
                  offset - safepointIndices_.back().displacement() >= sizeof(uint32_t));
    masm.propagateOOM(safepointIndices_.append(SafepointIndex(offset, ins->safepoint())));
}

void
CodeGeneratorShared::ensureOsiSpace()
{
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
    if (masm.currentOffset() - lastOsiPointOffset_ < Assembler::PatchWrite_NearCallSize()) {
        int32_t paddingSize = Assembler::PatchWrite_NearCallSize();
        paddingSize -= masm.currentOffset() - lastOsiPointOffset_;
        for (int32_t i = 0; i < paddingSize; ++i)
            masm.nop();
    }
    MOZ_ASSERT_IF(!masm.oom(),
                  masm.currentOffset() - lastOsiPointOffset_ >= Assembler::PatchWrite_NearCallSize());
    lastOsiPointOffset_ = masm.currentOffset();
}

uint32_t
CodeGeneratorShared::markOsiPoint(LOsiPoint* ins)
{
    encode(ins->snapshot());
    ensureOsiSpace();

    uint32_t offset = masm.currentOffset();
    SnapshotOffset so = ins->snapshot()->snapshotOffset();
    masm.propagateOOM(osiIndices_.append(OsiIndex(offset, so)));

    return offset;
}

#ifdef CHECK_OSIPOINT_REGISTERS
template <class Op>
static void
HandleRegisterDump(Op op, MacroAssembler& masm, LiveRegisterSet liveRegs, Register activation,
                   Register scratch)
{
    const size_t baseOffset = JitActivation::offsetOfRegs();

    // Handle live GPRs.
    for (GeneralRegisterIterator iter(liveRegs.gprs()); iter.more(); ++iter) {
        Register reg = *iter;
        Address dump(activation, baseOffset + RegisterDump::offsetOfRegister(reg));

        if (reg == activation) {
            // To use the original value of the activation register (that's
            // now on top of the stack), we need the scratch register.
            masm.push(scratch);
            masm.loadPtr(Address(masm.getStackPointer(), sizeof(uintptr_t)), scratch);
            op(scratch, dump);
            masm.pop(scratch);
        } else {
            op(reg, dump);
        }
    }

    // Handle live FPRs.
    for (FloatRegisterIterator iter(liveRegs.fpus()); iter.more(); ++iter) {
        FloatRegister reg = *iter;
        Address dump(activation, baseOffset + RegisterDump::offsetOfRegister(reg));
        op(reg, dump);
    }
}

class StoreOp
{
    MacroAssembler& masm;

  public:
    explicit StoreOp(MacroAssembler& masm)
      : masm(masm)
    {}

    void operator()(Register reg, Address dump) {
        masm.storePtr(reg, dump);
    }
    void operator()(FloatRegister reg, Address dump) {
        if (reg.isDouble())
            masm.storeDouble(reg, dump);
        else if (reg.isSingle())
            masm.storeFloat32(reg, dump);
#if defined(JS_CODEGEN_X86) || defined(JS_CODEGEN_X64)
        else if (reg.isSimd128())
            masm.storeUnalignedSimd128Float(reg, dump);
#endif
        else
            MOZ_CRASH("Unexpected register type.");
    }
};

static void
StoreAllLiveRegs(MacroAssembler& masm, LiveRegisterSet liveRegs)
{
    // Store a copy of all live registers before performing the call.
    // When we reach the OsiPoint, we can use this to check nothing
    // modified them in the meantime.

    // Load pointer to the JitActivation in a scratch register.
    AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
    Register scratch = allRegs.takeAny();
    masm.push(scratch);
    masm.loadJitActivation(scratch);

    Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
    masm.add32(Imm32(1), checkRegs);

    StoreOp op(masm);
    HandleRegisterDump<StoreOp>(op, masm, liveRegs, scratch, allRegs.getAny());

    masm.pop(scratch);
}

class VerifyOp
{
    MacroAssembler& masm;
    Label* failure_;

  public:
    VerifyOp(MacroAssembler& masm, Label* failure)
      : masm(masm), failure_(failure)
    {}

    void operator()(Register reg, Address dump) {
        masm.branchPtr(Assembler::NotEqual, dump, reg, failure_);
    }
    void operator()(FloatRegister reg, Address dump) {
        FloatRegister scratch;
        if (reg.isDouble()) {
            scratch = ScratchDoubleReg;
            masm.loadDouble(dump, scratch);
            masm.branchDouble(Assembler::DoubleNotEqual, scratch, reg, failure_);
        } else if (reg.isSingle()) {
            scratch = ScratchFloat32Reg;
            masm.loadFloat32(dump, scratch);
            masm.branchFloat(Assembler::DoubleNotEqual, scratch, reg, failure_);
        }

        // :TODO: (Bug 1133745) Add support to verify SIMD registers.
    }
};

void
CodeGeneratorShared::verifyOsiPointRegs(LSafepoint* safepoint)
{
    // Ensure the live registers stored by callVM did not change between
    // the call and this OsiPoint. Try-catch relies on this invariant.

    // Load pointer to the JitActivation in a scratch register.
    AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
    Register scratch = allRegs.takeAny();
    masm.push(scratch);
    masm.loadJitActivation(scratch);

    // If we should not check registers (because the instruction did not call
    // into the VM, or a GC happened), we're done.
    Label failure, done;
    Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
    masm.branch32(Assembler::Equal, checkRegs, Imm32(0), &done);

    // Having more than one VM function call made in one visit function at
    // runtime is a sec-ciritcal error, because if we conservatively assume that
    // one of the function call can re-enter Ion, then the invalidation process
    // will potentially add a call at a random location, by patching the code
    // before the return address.
    masm.branch32(Assembler::NotEqual, checkRegs, Imm32(1), &failure);

    // Set checkRegs to 0, so that we don't try to verify registers after we
    // return from this script to the caller.
    masm.store32(Imm32(0), checkRegs);

    // Ignore clobbered registers. Some instructions (like LValueToInt32) modify
    // temps after calling into the VM. This is fine because no other
    // instructions (including this OsiPoint) will depend on them. Also
    // backtracking can also use the same register for an input and an output.
    // These are marked as clobbered and shouldn't get checked.
    LiveRegisterSet liveRegs;
    liveRegs.set() = RegisterSet::Intersect(safepoint->liveRegs().set(),
                                            RegisterSet::Not(safepoint->clobberedRegs().set()));

    VerifyOp op(masm, &failure);
    HandleRegisterDump<VerifyOp>(op, masm, liveRegs, scratch, allRegs.getAny());

    masm.jump(&done);

    // Do not profile the callWithABI that occurs below.  This is to avoid a
    // rare corner case that occurs when profiling interacts with itself:
    //
    // When slow profiling assertions are turned on, FunctionBoundary ops
    // (which update the profiler pseudo-stack) may emit a callVM, which
    // forces them to have an osi point associated with them.  The
    // FunctionBoundary for inline function entry is added to the caller's
    // graph with a PC from the caller's code, but during codegen it modifies
    // Gecko Profiler instrumentation to add the callee as the current top-most
    // script. When codegen gets to the OSIPoint, and the callWithABI below is
    // emitted, the codegen thinks that the current frame is the callee, but
    // the PC it's using from the OSIPoint refers to the caller.  This causes
    // the profiler instrumentation of the callWithABI below to ASSERT, since
    // the script and pc are mismatched.  To avoid this, we simply omit
    // instrumentation for these callWithABIs.

    // Any live register captured by a safepoint (other than temp registers)
    // must remain unchanged between the call and the OsiPoint instruction.
    masm.bind(&failure);
    masm.assumeUnreachable("Modified registers between VM call and OsiPoint");

    masm.bind(&done);
    masm.pop(scratch);
}

bool
CodeGeneratorShared::shouldVerifyOsiPointRegs(LSafepoint* safepoint)
{
    if (!checkOsiPointRegisters)
        return false;

    if (safepoint->liveRegs().emptyGeneral() && safepoint->liveRegs().emptyFloat())
        return false; // No registers to check.

    return true;
}

void
CodeGeneratorShared::resetOsiPointRegs(LSafepoint* safepoint)
{
    if (!shouldVerifyOsiPointRegs(safepoint))
        return;

    // Set checkRegs to 0. If we perform a VM call, the instruction
    // will set it to 1.
    AllocatableGeneralRegisterSet allRegs(GeneralRegisterSet::All());
    Register scratch = allRegs.takeAny();
    masm.push(scratch);
    masm.loadJitActivation(scratch);
    Address checkRegs(scratch, JitActivation::offsetOfCheckRegs());
    masm.store32(Imm32(0), checkRegs);
    masm.pop(scratch);
}
#endif

// Before doing any call to Cpp, you should ensure that volatile
// registers are evicted by the register allocator.
void
CodeGeneratorShared::callVM(const VMFunction& fun, LInstruction* ins, const Register* dynStack)
{
    // If we're calling a function with an out parameter type of double, make
    // sure we have an FPU.
    MOZ_ASSERT_IF(fun.outParam == Type_Double, gen->runtime->jitSupportsFloatingPoint());

#ifdef DEBUG
    if (ins->mirRaw()) {
        MOZ_ASSERT(ins->mirRaw()->isInstruction());
        MInstruction* mir = ins->mirRaw()->toInstruction();
        MOZ_ASSERT_IF(mir->needsResumePoint(), mir->resumePoint());
    }
#endif

    // Stack is:
    //    ... frame ...
    //    [args]
#ifdef DEBUG
    MOZ_ASSERT(pushedArgs_ == fun.explicitArgs);
    pushedArgs_ = 0;
#endif

    // Get the wrapper of the VM function.
    TrampolinePtr wrapper = gen->jitRuntime()->getVMWrapper(fun);

#ifdef CHECK_OSIPOINT_REGISTERS
    if (shouldVerifyOsiPointRegs(ins->safepoint()))
        StoreAllLiveRegs(masm, ins->safepoint()->liveRegs());
#endif

    // Push an exit frame descriptor. If |dynStack| is a valid pointer to a
    // register, then its value is added to the value of the |framePushed()| to
    // fill the frame descriptor.
    if (dynStack) {
        masm.addPtr(Imm32(masm.framePushed()), *dynStack);
        masm.makeFrameDescriptor(*dynStack, JitFrame_IonJS, ExitFrameLayout::Size());
        masm.Push(*dynStack); // descriptor
    } else {
        masm.pushStaticFrameDescriptor(JitFrame_IonJS, ExitFrameLayout::Size());
    }

    // Call the wrapper function.  The wrapper is in charge to unwind the stack
    // when returning from the call.  Failures are handled with exceptions based
    // on the return value of the C functions.  To guard the outcome of the
    // returned value, use another LIR instruction.
    uint32_t callOffset = masm.callJit(wrapper);
    markSafepointAt(callOffset, ins);

    // Remove rest of the frame left on the stack. We remove the return address
    // which is implicitly poped when returning.
    int framePop = sizeof(ExitFrameLayout) - sizeof(void*);

    // Pop arguments from framePushed.
    masm.implicitPop(fun.explicitStackSlots() * sizeof(void*) + framePop);
    // Stack is:
    //    ... frame ...
}

class OutOfLineTruncateSlow : public OutOfLineCodeBase<CodeGeneratorShared>
{
    FloatRegister src_;
    Register dest_;
    bool widenFloatToDouble_;
    wasm::BytecodeOffset bytecodeOffset_;

  public:
    OutOfLineTruncateSlow(FloatRegister src, Register dest, bool widenFloatToDouble = false,
                          wasm::BytecodeOffset bytecodeOffset = wasm::BytecodeOffset())
      : src_(src),
        dest_(dest),
        widenFloatToDouble_(widenFloatToDouble),
        bytecodeOffset_(bytecodeOffset)
    { }

    void accept(CodeGeneratorShared* codegen) override {
        codegen->visitOutOfLineTruncateSlow(this);
    }
    FloatRegister src() const {
        return src_;
    }
    Register dest() const {
        return dest_;
    }
    bool widenFloatToDouble() const {
        return widenFloatToDouble_;
    }
    wasm::BytecodeOffset bytecodeOffset() const {
        return bytecodeOffset_;
    }
};

OutOfLineCode*
CodeGeneratorShared::oolTruncateDouble(FloatRegister src, Register dest, MInstruction* mir,
                                       wasm::BytecodeOffset bytecodeOffset)
{
    MOZ_ASSERT_IF(IsCompilingWasm(), bytecodeOffset.isValid());

    OutOfLineTruncateSlow* ool = new(alloc()) OutOfLineTruncateSlow(src, dest, /* float32 */ false,
                                                                    bytecodeOffset);
    addOutOfLineCode(ool, mir);
    return ool;
}

void
CodeGeneratorShared::emitTruncateDouble(FloatRegister src, Register dest, MTruncateToInt32* mir)
{
    OutOfLineCode* ool = oolTruncateDouble(src, dest, mir, mir->bytecodeOffset());

    masm.branchTruncateDoubleMaybeModUint32(src, dest, ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGeneratorShared::emitTruncateFloat32(FloatRegister src, Register dest, MTruncateToInt32* mir)
{
    OutOfLineTruncateSlow* ool = new(alloc()) OutOfLineTruncateSlow(src, dest, /* float32 */ true,
                                                                    mir->bytecodeOffset());
    addOutOfLineCode(ool, mir);

    masm.branchTruncateFloat32MaybeModUint32(src, dest, ool->entry());
    masm.bind(ool->rejoin());
}

void
CodeGeneratorShared::visitOutOfLineTruncateSlow(OutOfLineTruncateSlow* ool)
{
    FloatRegister src = ool->src();
    Register dest = ool->dest();

    saveVolatile(dest);
    masm.outOfLineTruncateSlow(src, dest, ool->widenFloatToDouble(), gen->compilingWasm(),
                               ool->bytecodeOffset());
    restoreVolatile(dest);

    masm.jump(ool->rejoin());
}

bool
CodeGeneratorShared::omitOverRecursedCheck() const
{
    // If the current function makes no calls (which means it isn't recursive)
    // and it uses only a small amount of stack space, it doesn't need a
    // stack overflow check. Note that the actual number here is somewhat
    // arbitrary, and codegen actually uses small bounded amounts of
    // additional stack space in some cases too.
    return frameSize() < MAX_UNCHECKED_LEAF_FRAME_SIZE && !gen->needsOverrecursedCheck();
}

void
CodeGeneratorShared::emitWasmCallBase(MWasmCall* mir, bool needsBoundsCheck)
{
    if (mir->spIncrement())
        masm.freeStack(mir->spIncrement());

    MOZ_ASSERT((sizeof(wasm::Frame) + masm.framePushed()) % WasmStackAlignment == 0);
    static_assert(WasmStackAlignment >= ABIStackAlignment &&
                  WasmStackAlignment % ABIStackAlignment == 0,
                  "The wasm stack alignment should subsume the ABI-required alignment");

#ifdef DEBUG
    Label ok;
    masm.branchTestStackPtr(Assembler::Zero, Imm32(WasmStackAlignment - 1), &ok);
    masm.breakpoint();
    masm.bind(&ok);
#endif

    // LWasmCallBase::isCallPreserved() assumes that all MWasmCalls preserve the
    // TLS and pinned regs. The only case where where we don't have to reload
    // the TLS and pinned regs is when the callee preserves them.
    bool reloadRegs = true;

    const wasm::CallSiteDesc& desc = mir->desc();
    const wasm::CalleeDesc& callee = mir->callee();
    switch (callee.which()) {
      case wasm::CalleeDesc::Func:
        masm.call(desc, callee.funcIndex());
        reloadRegs = false;
        break;
      case wasm::CalleeDesc::Import:
        masm.wasmCallImport(desc, callee);
        break;
      case wasm::CalleeDesc::AsmJSTable:
      case wasm::CalleeDesc::WasmTable:
        masm.wasmCallIndirect(desc, callee, needsBoundsCheck);
        reloadRegs = callee.which() == wasm::CalleeDesc::WasmTable && callee.wasmTableIsExternal();
        break;
      case wasm::CalleeDesc::Builtin:
        masm.call(desc, callee.builtin());
        reloadRegs = false;
        break;
      case wasm::CalleeDesc::BuiltinInstanceMethod:
        masm.wasmCallBuiltinInstanceMethod(desc, mir->instanceArg(), callee.builtin());
        break;
    }

    if (reloadRegs) {
        masm.loadWasmTlsRegFromFrame();
        masm.loadWasmPinnedRegsFromTls();
    }

    if (mir->spIncrement())
        masm.reserveStack(mir->spIncrement());
}

void
CodeGeneratorShared::visitWasmLoadGlobalVar(LWasmLoadGlobalVar* ins)
{
    MWasmLoadGlobalVar* mir = ins->mir();

    MIRType type = mir->type();
    MOZ_ASSERT(IsNumberType(type) || IsSimdType(type));

    Register tls = ToRegister(ins->tlsPtr());
    Address addr(tls, offsetof(wasm::TlsData, globalArea) + mir->globalDataOffset());
    switch (type) {
      case MIRType::Int32:
        masm.load32(addr, ToRegister(ins->output()));
        break;
      case MIRType::Float32:
        masm.loadFloat32(addr, ToFloatRegister(ins->output()));
        break;
      case MIRType::Double:
        masm.loadDouble(addr, ToFloatRegister(ins->output()));
        break;
      // Aligned access: code is aligned on PageSize + there is padding
      // before the global data section.
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        masm.loadInt32x4(addr, ToFloatRegister(ins->output()));
        break;
      case MIRType::Float32x4:
        masm.loadFloat32x4(addr, ToFloatRegister(ins->output()));
        break;
      default:
        MOZ_CRASH("unexpected type in visitWasmLoadGlobalVar");
    }
}

void
CodeGeneratorShared::visitWasmStoreGlobalVar(LWasmStoreGlobalVar* ins)
{
    MWasmStoreGlobalVar* mir = ins->mir();

    MIRType type = mir->value()->type();
    MOZ_ASSERT(IsNumberType(type) || IsSimdType(type));

    Register tls = ToRegister(ins->tlsPtr());
    Address addr(tls, offsetof(wasm::TlsData, globalArea) + mir->globalDataOffset());
    switch (type) {
      case MIRType::Int32:
        masm.store32(ToRegister(ins->value()), addr);
        break;
      case MIRType::Float32:
        masm.storeFloat32(ToFloatRegister(ins->value()), addr);
        break;
      case MIRType::Double:
        masm.storeDouble(ToFloatRegister(ins->value()), addr);
        break;
      // Aligned access: code is aligned on PageSize + there is padding
      // before the global data section.
      case MIRType::Int8x16:
      case MIRType::Int16x8:
      case MIRType::Int32x4:
      case MIRType::Bool8x16:
      case MIRType::Bool16x8:
      case MIRType::Bool32x4:
        masm.storeInt32x4(ToFloatRegister(ins->value()), addr);
        break;
      case MIRType::Float32x4:
        masm.storeFloat32x4(ToFloatRegister(ins->value()), addr);
        break;
      default:
        MOZ_CRASH("unexpected type in visitWasmStoreGlobalVar");
    }
}

void
CodeGeneratorShared::visitWasmLoadGlobalVarI64(LWasmLoadGlobalVarI64* ins)
{
    MWasmLoadGlobalVar* mir = ins->mir();
    MOZ_ASSERT(mir->type() == MIRType::Int64);

    Register tls = ToRegister(ins->tlsPtr());
    Address addr(tls, offsetof(wasm::TlsData, globalArea) + mir->globalDataOffset());

    Register64 output = ToOutRegister64(ins);
    masm.load64(addr, output);
}

void
CodeGeneratorShared::visitWasmStoreGlobalVarI64(LWasmStoreGlobalVarI64* ins)
{
    MWasmStoreGlobalVar* mir = ins->mir();
    MOZ_ASSERT(mir->value()->type() == MIRType::Int64);

    Register tls = ToRegister(ins->tlsPtr());
    Address addr(tls, offsetof(wasm::TlsData, globalArea) + mir->globalDataOffset());

    Register64 value = ToRegister64(ins->value());
    masm.store64(value, addr);
}

void
CodeGeneratorShared::emitPreBarrier(Register base, const LAllocation* index, int32_t offsetAdjustment)
{
    if (index->isConstant()) {
        Address address(base, ToInt32(index) * sizeof(Value) + offsetAdjustment);
        masm.guardedCallPreBarrier(address, MIRType::Value);
    } else {
        BaseIndex address(base, ToRegister(index), TimesEight, offsetAdjustment);
        masm.guardedCallPreBarrier(address, MIRType::Value);
    }
}

void
CodeGeneratorShared::emitPreBarrier(Address address)
{
    masm.guardedCallPreBarrier(address, MIRType::Value);
}

Label*
CodeGeneratorShared::labelForBackedgeWithImplicitCheck(MBasicBlock* mir)
{
    // If this is a loop backedge to a loop header with an implicit interrupt
    // check, use a patchable jump. Skip this search if compiling without a
    // script for wasm, as there will be no interrupt check instruction.
    // Due to critical edge unsplitting there may no longer be unique loop
    // backedges, so just look for any edge going to an earlier block in RPO.
    if (!gen->compilingWasm() && mir->isLoopHeader() && mir->id() <= current->mir()->id()) {
        for (LInstructionIterator iter = mir->lir()->begin(); iter != mir->lir()->end(); iter++) {
            if (iter->isMoveGroup()) {
                // Continue searching for an interrupt check.
            } else {
                // The interrupt check should be the first instruction in the
                // loop header other than move groups.
                MOZ_ASSERT(iter->isInterruptCheck());
                if (iter->toInterruptCheck()->implicit())
                    return iter->toInterruptCheck()->oolEntry();
                return nullptr;
            }
        }
    }

    return nullptr;
}

void
CodeGeneratorShared::jumpToBlock(MBasicBlock* mir)
{
    // Skip past trivial blocks.
    mir = skipTrivialBlocks(mir);

    // No jump necessary if we can fall through to the next block.
    if (isNextBlock(mir->lir()))
        return;

    if (Label* oolEntry = labelForBackedgeWithImplicitCheck(mir)) {
        // Note: the backedge is initially a jump to the next instruction.
        // It will be patched to the target block's label during link().
        RepatchLabel rejoin;
        CodeOffsetJump backedge = masm.backedgeJump(&rejoin, mir->lir()->label());
        masm.bind(&rejoin);

        masm.propagateOOM(patchableBackedges_.append(PatchableBackedgeInfo(backedge, mir->lir()->label(), oolEntry)));
    } else {
        masm.jump(mir->lir()->label());
    }
}

Label*
CodeGeneratorShared::getJumpLabelForBranch(MBasicBlock* block)
{
    // Skip past trivial blocks.
    block = skipTrivialBlocks(block);

    if (!labelForBackedgeWithImplicitCheck(block))
        return block->lir()->label();

    // We need to use a patchable jump for this backedge, but want to treat
    // this as a normal label target to simplify codegen. Efficiency isn't so
    // important here as these tests are extremely unlikely to be used in loop
    // backedges, so emit inline code for the patchable jump. Heap allocating
    // the label allows it to be used by out of line blocks.
    Label* res = alloc().lifoAlloc()->newInfallible<Label>();
    Label after;
    masm.jump(&after);
    masm.bind(res);
    jumpToBlock(block);
    masm.bind(&after);
    return res;
}

// This function is not used for MIPS/MIPS64. MIPS has branchToBlock.
#if !defined(JS_CODEGEN_MIPS32) && !defined(JS_CODEGEN_MIPS64)
void
CodeGeneratorShared::jumpToBlock(MBasicBlock* mir, Assembler::Condition cond)
{
    // Skip past trivial blocks.
    mir = skipTrivialBlocks(mir);

    if (Label* oolEntry = labelForBackedgeWithImplicitCheck(mir)) {
        // Note: the backedge is initially a jump to the next instruction.
        // It will be patched to the target block's label during link().
        RepatchLabel rejoin;
        CodeOffsetJump backedge = masm.jumpWithPatch(&rejoin, cond, mir->lir()->label());
        masm.bind(&rejoin);

        masm.propagateOOM(patchableBackedges_.append(PatchableBackedgeInfo(backedge, mir->lir()->label(), oolEntry)));
    } else {
        masm.j(cond, mir->lir()->label());
    }
}
#endif

ReciprocalMulConstants
CodeGeneratorShared::computeDivisionConstants(uint32_t d, int maxLog) {
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
    while ((uint64_t(1) << (p-maxLog)) + (UINT64_MAX >> (64-p)) % d + 1 < d)
        p++;

    // ...and the corresponding M. For either the signed (L=31) or the
    // unsigned (L=32) case, this value can be too large (cf. item a).
    // Codegen can still multiply by M by multiplying by (M - 2^L) and
    // adjusting the value afterwards, if this is the case.
    ReciprocalMulConstants rmc;
    rmc.multiplier = (UINT64_MAX >> (64-p))/d + 1;
    rmc.shiftAmount = p - 32;

    return rmc;
}

#ifdef JS_TRACE_LOGGING

void
CodeGeneratorShared::emitTracelogScript(bool isStart)
{
    if (!TraceLogTextIdEnabled(TraceLogger_Scripts))
        return;

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

    if (isStart)
        masm.tracelogStartId(logger, script);
    else
        masm.tracelogStopId(logger, script);

    masm.Pop(script);

    masm.bind(&done);

    masm.Pop(logger);
}

void
CodeGeneratorShared::emitTracelogTree(bool isStart, uint32_t textId)
{
    if (!TraceLogTextIdEnabled(textId))
        return;

    Label done;
    AllocatableRegisterSet regs(RegisterSet::Volatile());
    Register logger = regs.takeAnyGeneral();

    masm.Push(logger);

    masm.loadTraceLogger(logger);
    masm.branchTestPtr(Assembler::Zero, logger, logger, &done);

    Address enabledAddress(logger, TraceLoggerThread::offsetOfEnabled());
    masm.branch32(Assembler::Equal, enabledAddress, Imm32(0), &done);

    if (isStart)
        masm.tracelogStartId(logger, textId);
    else
        masm.tracelogStopId(logger, textId);

    masm.bind(&done);

    masm.Pop(logger);
}

void
CodeGeneratorShared::emitTracelogTree(bool isStart, const char* text,
                                      TraceLoggerTextId enabledTextId)
{
    if (!TraceLogTextIdEnabled(enabledTextId))
        return;

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
    masm.propagateOOM(patchableTLEvents_.append(Move(patchEvent)));

    if (isStart)
        masm.tracelogStartId(loggerReg, eventReg);
    else
        masm.tracelogStopId(loggerReg, eventReg);

    masm.Pop(eventReg);

    masm.bind(&done);

    masm.Pop(loggerReg);
}
#endif

} // namespace jit
} // namespace js
