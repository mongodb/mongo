/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/JitcodeMap.h"

#include "mozilla/DebugOnly.h"
#include "mozilla/UniquePtr.h"
#include "jsprf.h"

#include "jit/BaselineJIT.h"
#include "jit/JitSpewer.h"

#include "js/Vector.h"
#include "vm/SPSProfiler.h"
#include "jsscriptinlines.h"

namespace js {
namespace jit {

bool
JitcodeGlobalEntry::IonEntry::callStackAtAddr(JSRuntime* rt, void* ptr,
                                              BytecodeLocationVector& results,
                                              uint32_t* depth) const
{
    MOZ_ASSERT(containsPointer(ptr));
    uint32_t ptrOffset = reinterpret_cast<uint8_t*>(ptr) -
                         reinterpret_cast<uint8_t*>(nativeStartAddr());

    uint32_t regionIdx = regionTable()->findRegionEntry(ptrOffset);
    MOZ_ASSERT(regionIdx < regionTable()->numRegions());

    JitcodeRegionEntry region = regionTable()->regionEntry(regionIdx);
    *depth = region.scriptDepth();

    JitcodeRegionEntry::ScriptPcIterator locationIter = region.scriptPcIterator();
    MOZ_ASSERT(locationIter.hasMore());
    bool first = true;
    while (locationIter.hasMore()) {
        uint32_t scriptIdx, pcOffset;
        locationIter.readNext(&scriptIdx, &pcOffset);
        // For the first entry pushed (innermost frame), the pcOffset is obtained
        // from the delta-run encodings.
        if (first) {
            pcOffset = region.findPcOffset(ptrOffset, pcOffset);
            first = false;
        }
        JSScript* script = getScript(scriptIdx);
        jsbytecode* pc = script->offsetToPC(pcOffset);
        if (!results.append(BytecodeLocation(script, pc)))
            return false;
    }

    return true;
}

uint32_t
JitcodeGlobalEntry::IonEntry::callStackAtAddr(JSRuntime* rt, void* ptr,
                                              const char** results,
                                              uint32_t maxResults) const
{
    MOZ_ASSERT(containsPointer(ptr));
    MOZ_ASSERT(maxResults >= 1);
    uint32_t ptrOffset = reinterpret_cast<uint8_t*>(ptr) -
                         reinterpret_cast<uint8_t*>(nativeStartAddr());

    uint32_t regionIdx = regionTable()->findRegionEntry(ptrOffset);
    MOZ_ASSERT(regionIdx < regionTable()->numRegions());

    JitcodeRegionEntry region = regionTable()->regionEntry(regionIdx);

    JitcodeRegionEntry::ScriptPcIterator locationIter = region.scriptPcIterator();
    MOZ_ASSERT(locationIter.hasMore());
    uint32_t count = 0;
    while (locationIter.hasMore()) {
        uint32_t scriptIdx, pcOffset;

        locationIter.readNext(&scriptIdx, &pcOffset);
        MOZ_ASSERT(getStr(scriptIdx));

        results[count++] = getStr(scriptIdx);
        if (count >= maxResults)
            break;
    }

    return count;
}

void
JitcodeGlobalEntry::IonEntry::destroy()
{
    // The region table is stored at the tail of the compacted data,
    // which means the start of the region table is a pointer to
    // the _middle_ of the memory space allocated for it.
    //
    // When freeing it, obtain the payload start pointer first.
    if (regionTable_)
        js_free((void*) (regionTable_->payloadStart()));
    regionTable_ = nullptr;

    // Free the scriptList strs.
    for (uint32_t i = 0; i < scriptList_->size; i++)  {
        js_free(scriptList_->pairs[i].str);
        scriptList_->pairs[i].str = nullptr;
    }

    // Free the script list
    js_free(scriptList_);
    scriptList_ = nullptr;

    // The optimizations region and attempts table is in the same block of
    // memory, the beginning of which is pointed to by
    // optimizationsRegionTable_->payloadStart().
    if (optsRegionTable_) {
        MOZ_ASSERT(optsAttemptsTable_);
        js_free((void*) optsRegionTable_->payloadStart());
    }
    optsRegionTable_ = nullptr;
    optsTypesTable_ = nullptr;
    optsAttemptsTable_ = nullptr;
    js_delete(optsAllTypes_);
    optsAllTypes_ = nullptr;
}

bool
JitcodeGlobalEntry::BaselineEntry::callStackAtAddr(JSRuntime* rt, void* ptr,
                                                   BytecodeLocationVector& results,
                                                   uint32_t* depth) const
{
    MOZ_ASSERT(containsPointer(ptr));
    MOZ_ASSERT(script_->hasBaselineScript());

    uint8_t* addr = reinterpret_cast<uint8_t*>(ptr);
    jsbytecode* pc = script_->baselineScript()->approximatePcForNativeAddress(script_, addr);
    if (!results.append(BytecodeLocation(script_, pc)))
        return false;

    *depth = 1;

    return true;
}

uint32_t
JitcodeGlobalEntry::BaselineEntry::callStackAtAddr(JSRuntime* rt, void* ptr,
                                                   const char** results,
                                                   uint32_t maxResults) const
{
    MOZ_ASSERT(containsPointer(ptr));
    MOZ_ASSERT(script_->hasBaselineScript());
    MOZ_ASSERT(maxResults >= 1);

    results[0] = str();
    return 1;
}

void
JitcodeGlobalEntry::BaselineEntry::destroy()
{
    if (!str_)
        return;
    js_free((void*) str_);
    str_ = nullptr;
}

bool
JitcodeGlobalEntry::IonCacheEntry::callStackAtAddr(JSRuntime* rt, void* ptr,
                                                   BytecodeLocationVector& results,
                                                   uint32_t* depth) const
{
    MOZ_ASSERT(containsPointer(ptr));

    // There must exist an entry for the rejoin addr if this entry exists.
    JitRuntime* jitrt = rt->jitRuntime();
    JitcodeGlobalEntry entry;
    jitrt->getJitcodeGlobalTable()->lookupInfallible(rejoinAddr(), &entry, rt);
    MOZ_ASSERT(entry.isIon());

    return entry.callStackAtAddr(rt, rejoinAddr(), results, depth);
}

uint32_t
JitcodeGlobalEntry::IonCacheEntry::callStackAtAddr(JSRuntime* rt, void* ptr,
                                                   const char** results,
                                                   uint32_t maxResults) const
{
    MOZ_ASSERT(containsPointer(ptr));

    // There must exist an entry for the rejoin addr if this entry exists.
    JitRuntime* jitrt = rt->jitRuntime();
    JitcodeGlobalEntry entry;
    jitrt->getJitcodeGlobalTable()->lookupInfallible(rejoinAddr(), &entry, rt);
    MOZ_ASSERT(entry.isIon());

    return entry.callStackAtAddr(rt, rejoinAddr(), results, maxResults);
}


static int ComparePointers(const void* a, const void* b) {
    const uint8_t* a_ptr = reinterpret_cast<const uint8_t*>(a);
    const uint8_t* b_ptr = reinterpret_cast<const uint8_t*>(b);
    if (a_ptr < b_ptr)
        return -1;
    if (a_ptr > b_ptr)
        return 1;
    return 0;
}

/* static */ int
JitcodeGlobalEntry::compare(const JitcodeGlobalEntry& ent1, const JitcodeGlobalEntry& ent2)
{
    // Both parts of compare cannot be a query.
    MOZ_ASSERT(!(ent1.isQuery() && ent2.isQuery()));

    // Ensure no overlaps for non-query lookups.
    MOZ_ASSERT_IF(!ent1.isQuery() && !ent2.isQuery(), !ent1.overlapsWith(ent2));

    // For two non-query entries, just comapare the start addresses.
    if (!ent1.isQuery() && !ent2.isQuery())
        return ComparePointers(ent1.nativeStartAddr(), ent2.nativeStartAddr());

    void* ptr = ent1.isQuery() ? ent1.nativeStartAddr() : ent2.nativeStartAddr();
    const JitcodeGlobalEntry& ent = ent1.isQuery() ? ent2 : ent1;
    int flip = ent1.isQuery() ? 1 : -1;

    if (ent.startsBelowPointer(ptr)) {
        if (ent.endsAbovePointer(ptr))
            return 0;

        // query ptr > entry
        return flip * 1;
    }

    // query ptr < entry
    return flip * -1;
}

/* static */ char*
JitcodeGlobalEntry::createScriptString(JSContext* cx, JSScript* script, size_t* length)
{
    // If the script has a function, try calculating its name.
    bool hasName = false;
    size_t nameLength = 0;
    mozilla::UniquePtr<char, JS::FreePolicy> nameStr = nullptr;
    JSFunction* func = script->functionDelazifying();
    if (func && func->displayAtom()) {
        JSAtom* atom = func->displayAtom();

        JS::AutoCheckCannotGC nogc;
        nameStr = mozilla::UniquePtr<char, JS::FreePolicy>(
            atom->hasLatin1Chars() ?
                JS::CharsToNewUTF8CharsZ(cx, atom->latin1Range(nogc)).c_str()
              : JS::CharsToNewUTF8CharsZ(cx, atom->twoByteRange(nogc)).c_str());
        if (!nameStr)
            return nullptr;

        nameLength = strlen(nameStr.get());
        hasName = true;
    }

    // Calculate filename length
    const char* filenameStr = script->filename() ? script->filename() : "(null)";
    size_t filenameLength = strlen(filenameStr);

    // Calculate lineno length
    bool hasLineno = false;
    size_t linenoLength = 0;
    char linenoStr[15];
    if (hasName || (script->functionNonDelazifying() || script->isForEval())) {
        linenoLength = JS_snprintf(linenoStr, 15, "%u", (unsigned) script->lineno());
        hasLineno = true;
    }

    // Full profile string for scripts with functions is:
    //      FuncName (FileName:Lineno)
    // Full profile string for scripts without functions is:
    //      FileName:Lineno
    // Full profile string for scripts without functions and without linenos is:
    //      FileName

    // Calculate full string length.
    size_t fullLength = 0;
    if (hasName) {
        MOZ_ASSERT(hasLineno);
        fullLength = nameLength + 2 + filenameLength + 1 + linenoLength + 1;
    } else if (hasLineno) {
        fullLength = filenameLength + 1 + linenoLength;
    } else {
        fullLength = filenameLength;
    }

    // Allocate string.
    char* str = cx->pod_malloc<char>(fullLength + 1);
    if (!str)
        return nullptr;

    size_t cur = 0;

    // Fill string with func name if needed.
    if (hasName) {
        memcpy(str + cur, nameStr.get(), nameLength);
        cur += nameLength;
        str[cur++] = ' ';
        str[cur++] = '(';
    }

    // Fill string with filename chars.
    memcpy(str + cur, filenameStr, filenameLength);
    cur += filenameLength;

    // Fill lineno chars.
    if (hasLineno) {
        str[cur++] = ':';
        memcpy(str + cur, linenoStr, linenoLength);
        cur += linenoLength;
    }

    // Terminal ')' if necessary.
    if (hasName)
        str[cur++] = ')';

    MOZ_ASSERT(cur == fullLength);
    str[cur] = 0;

    if (length)
        *length = fullLength;

    return str;
}

bool
JitcodeGlobalTable::lookup(void* ptr, JitcodeGlobalEntry* result, JSRuntime* rt)
{
    MOZ_ASSERT(result);

    // Construct a JitcodeGlobalEntry::Query to do the lookup
    JitcodeGlobalEntry query = JitcodeGlobalEntry::MakeQuery(ptr);

    // Lookups on tree does mutation.  Suppress sampling when this is happening.
    AutoSuppressProfilerSampling suppressSampling(rt);
    return tree_.contains(query, result);
}

void
JitcodeGlobalTable::lookupInfallible(void* ptr, JitcodeGlobalEntry* result, JSRuntime* rt)
{
    mozilla::DebugOnly<bool> success = lookup(ptr, result, rt);
    MOZ_ASSERT(success);
}

bool
JitcodeGlobalTable::addEntry(const JitcodeGlobalEntry& entry, JSRuntime* rt)
{
    // Suppress profiler sampling while table is being mutated.
    AutoSuppressProfilerSampling suppressSampling(rt);

    MOZ_ASSERT(entry.isIon() || entry.isBaseline() || entry.isIonCache() || entry.isDummy());
    return tree_.insert(entry);
}

void
JitcodeGlobalTable::removeEntry(void* startAddr, JSRuntime* rt)
{
    // Suppress profiler sampling while table is being mutated.
    AutoSuppressProfilerSampling suppressSampling(rt);

    JitcodeGlobalEntry query = JitcodeGlobalEntry::MakeQuery(startAddr);
    JitcodeGlobalEntry result;
    mozilla::DebugOnly<bool> success = tree_.contains(query, &result);
    MOZ_ASSERT(success);

    // Destroy entry before removing it from tree.
    result.destroy();
    tree_.remove(query);
}

/* static */ void
JitcodeRegionEntry::WriteHead(CompactBufferWriter& writer,
                              uint32_t nativeOffset, uint8_t scriptDepth)
{
    writer.writeUnsigned(nativeOffset);
    writer.writeByte(scriptDepth);
}

/* static */ void
JitcodeRegionEntry::ReadHead(CompactBufferReader& reader,
                             uint32_t* nativeOffset, uint8_t* scriptDepth)
{
    *nativeOffset = reader.readUnsigned();
    *scriptDepth = reader.readByte();
}

/* static */ void
JitcodeRegionEntry::WriteScriptPc(CompactBufferWriter& writer,
                                  uint32_t scriptIdx, uint32_t pcOffset)
{
    writer.writeUnsigned(scriptIdx);
    writer.writeUnsigned(pcOffset);
}

/* static */ void
JitcodeRegionEntry::ReadScriptPc(CompactBufferReader& reader,
                                 uint32_t* scriptIdx, uint32_t* pcOffset)
{
    *scriptIdx = reader.readUnsigned();
    *pcOffset = reader.readUnsigned();
}

/* static */ void
JitcodeRegionEntry::WriteDelta(CompactBufferWriter& writer,
                               uint32_t nativeDelta, int32_t pcDelta)
{
    if (pcDelta >= 0) {
        // 1 and 2-byte formats possible.

        //  NNNN-BBB0
        if (pcDelta <= ENC1_PC_DELTA_MAX && nativeDelta <= ENC1_NATIVE_DELTA_MAX) {
            uint8_t encVal = ENC1_MASK_VAL | (pcDelta << ENC1_PC_DELTA_SHIFT) |
                             (nativeDelta << ENC1_NATIVE_DELTA_SHIFT);
            writer.writeByte(encVal);
            return;
        }

        //  NNNN-NNNN BBBB-BB01
        if (pcDelta <= ENC2_PC_DELTA_MAX && nativeDelta <= ENC2_NATIVE_DELTA_MAX) {
            uint16_t encVal = ENC2_MASK_VAL | (pcDelta << ENC2_PC_DELTA_SHIFT) |
                              (nativeDelta << ENC2_NATIVE_DELTA_SHIFT);
            writer.writeByte(encVal & 0xff);
            writer.writeByte((encVal >> 8) & 0xff);
            return;
        }
    }

    //  NNNN-NNNN NNNB-BBBB BBBB-B011
    if (pcDelta >= ENC3_PC_DELTA_MIN && pcDelta <= ENC3_PC_DELTA_MAX &&
        nativeDelta <= ENC3_NATIVE_DELTA_MAX)
    {
        uint32_t encVal = ENC3_MASK_VAL |
                          ((pcDelta << ENC3_PC_DELTA_SHIFT) & ENC3_PC_DELTA_MASK) |
                          (nativeDelta << ENC3_NATIVE_DELTA_SHIFT);
        writer.writeByte(encVal & 0xff);
        writer.writeByte((encVal >> 8) & 0xff);
        writer.writeByte((encVal >> 16) & 0xff);
        return;
    }

    //  NNNN-NNNN NNNN-NNNN BBBB-BBBB BBBB-B111
    if (pcDelta >= ENC4_PC_DELTA_MIN && pcDelta <= ENC4_PC_DELTA_MAX &&
        nativeDelta <= ENC4_NATIVE_DELTA_MAX)
    {
        uint32_t encVal = ENC4_MASK_VAL |
                          ((pcDelta << ENC4_PC_DELTA_SHIFT) & ENC4_PC_DELTA_MASK) |
                          (nativeDelta << ENC4_NATIVE_DELTA_SHIFT);
        writer.writeByte(encVal & 0xff);
        writer.writeByte((encVal >> 8) & 0xff);
        writer.writeByte((encVal >> 16) & 0xff);
        writer.writeByte((encVal >> 24) & 0xff);
        return;
    }

    // Should never get here.
    MOZ_CRASH("pcDelta/nativeDelta values are too large to encode.");
}

/* static */ void
JitcodeRegionEntry::ReadDelta(CompactBufferReader& reader,
                              uint32_t* nativeDelta, int32_t* pcDelta)
{
    // NB:
    // It's possible to get nativeDeltas with value 0 in two cases:
    //
    // 1. The last region's run.  This is because the region table's start
    // must be 4-byte aligned, and we must insert padding bytes to align the
    // payload section before emitting the table.
    //
    // 2. A zero-offset nativeDelta with a negative pcDelta.
    //
    // So if nativeDelta is zero, then pcDelta must be <= 0.

    //  NNNN-BBB0
    const uint32_t firstByte = reader.readByte();
    if ((firstByte & ENC1_MASK) == ENC1_MASK_VAL) {
        uint32_t encVal = firstByte;
        *nativeDelta = encVal >> ENC1_NATIVE_DELTA_SHIFT;
        *pcDelta = (encVal & ENC1_PC_DELTA_MASK) >> ENC1_PC_DELTA_SHIFT;
        MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
        return;
    }

    //  NNNN-NNNN BBBB-BB01
    const uint32_t secondByte = reader.readByte();
    if ((firstByte & ENC2_MASK) == ENC2_MASK_VAL) {
        uint32_t encVal = firstByte | secondByte << 8;
        *nativeDelta = encVal >> ENC2_NATIVE_DELTA_SHIFT;
        *pcDelta = (encVal & ENC2_PC_DELTA_MASK) >> ENC2_PC_DELTA_SHIFT;
        MOZ_ASSERT(*pcDelta != 0);
        MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
        return;
    }

    //  NNNN-NNNN NNNB-BBBB BBBB-B011
    const uint32_t thirdByte = reader.readByte();
    if ((firstByte & ENC3_MASK) == ENC3_MASK_VAL) {
        uint32_t encVal = firstByte | secondByte << 8 | thirdByte << 16;
        *nativeDelta = encVal >> ENC3_NATIVE_DELTA_SHIFT;

        uint32_t pcDeltaU = (encVal & ENC3_PC_DELTA_MASK) >> ENC3_PC_DELTA_SHIFT;
        // Fix sign if necessary.
        if (pcDeltaU > static_cast<uint32_t>(ENC3_PC_DELTA_MAX))
            pcDeltaU |= ~ENC3_PC_DELTA_MAX;
        *pcDelta = pcDeltaU;
        MOZ_ASSERT(*pcDelta != 0);
        MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
        return;
    }

    //  NNNN-NNNN NNNN-NNNN BBBB-BBBB BBBB-B111
    MOZ_ASSERT((firstByte & ENC4_MASK) == ENC4_MASK_VAL);
    const uint32_t fourthByte = reader.readByte();
    uint32_t encVal = firstByte | secondByte << 8 | thirdByte << 16 | fourthByte << 24;
    *nativeDelta = encVal >> ENC4_NATIVE_DELTA_SHIFT;

    uint32_t pcDeltaU = (encVal & ENC4_PC_DELTA_MASK) >> ENC4_PC_DELTA_SHIFT;
    // fix sign if necessary
    if (pcDeltaU > static_cast<uint32_t>(ENC4_PC_DELTA_MAX))
        pcDeltaU |= ~ENC4_PC_DELTA_MAX;
    *pcDelta = pcDeltaU;

    MOZ_ASSERT(*pcDelta != 0);
    MOZ_ASSERT_IF(*nativeDelta == 0, *pcDelta <= 0);
}

/* static */ uint32_t
JitcodeRegionEntry::ExpectedRunLength(const CodeGeneratorShared::NativeToBytecode* entry,
                                      const CodeGeneratorShared::NativeToBytecode* end)
{
    MOZ_ASSERT(entry < end);

    // We always use the first entry, so runLength starts at 1
    uint32_t runLength = 1;

    uint32_t curNativeOffset = entry->nativeOffset.offset();
    uint32_t curBytecodeOffset = entry->tree->script()->pcToOffset(entry->pc);

    for (auto nextEntry = entry + 1; nextEntry != end; nextEntry += 1) {
        // If the next run moves to a different inline site, stop the run.
        if (nextEntry->tree != entry->tree)
            break;

        uint32_t nextNativeOffset = nextEntry->nativeOffset.offset();
        uint32_t nextBytecodeOffset = nextEntry->tree->script()->pcToOffset(nextEntry->pc);
        MOZ_ASSERT(nextNativeOffset >= curNativeOffset);

        uint32_t nativeDelta = nextNativeOffset - curNativeOffset;
        int32_t bytecodeDelta = int32_t(nextBytecodeOffset) - int32_t(curBytecodeOffset);

        // If deltas are too large (very unlikely), stop the run.
        if (!IsDeltaEncodeable(nativeDelta, bytecodeDelta))
            break;

        runLength++;

        // If the run has grown to its maximum length, stop the run.
        if (runLength == MAX_RUN_LENGTH)
            break;

        curNativeOffset = nextNativeOffset;
        curBytecodeOffset = nextBytecodeOffset;
    }

    return runLength;
}

struct JitcodeMapBufferWriteSpewer
{
#ifdef DEBUG
    CompactBufferWriter* writer;
    uint32_t startPos;

    static const uint32_t DumpMaxBytes = 50;

    explicit JitcodeMapBufferWriteSpewer(CompactBufferWriter& w)
      : writer(&w), startPos(writer->length())
    {}

    void spewAndAdvance(const char* name) {
        uint32_t curPos = writer->length();
        const uint8_t* start = writer->buffer() + startPos;
        const uint8_t* end = writer->buffer() + curPos;
        const char* MAP = "0123456789ABCDEF";
        uint32_t bytes = end - start;

        char buffer[DumpMaxBytes * 3];
        for (uint32_t i = 0; i < bytes; i++) {
            buffer[i*3] = MAP[(start[i] >> 4) & 0xf];
            buffer[i*3 + 1] = MAP[(start[i] >> 0) & 0xf];
            buffer[i*3 + 2] = ' ';
        }
        if (bytes >= DumpMaxBytes)
            buffer[DumpMaxBytes*3 - 1] = '\0';
        else
            buffer[bytes*3 - 1] = '\0';

        JitSpew(JitSpew_Profiling, "%s@%d[%d bytes] - %s", name, int(startPos), int(bytes), buffer);

        // Move to the end of the current buffer.
        startPos = writer->length();
    }
#else // !DEBUG
    JitcodeMapBufferWriteSpewer(CompactBufferWriter& w) {}
    void spewAndAdvance(const char* name) {}
#endif // DEBUG
};

// Write a run, starting at the given NativeToBytecode entry, into the given buffer writer.
/* static */ bool
JitcodeRegionEntry::WriteRun(CompactBufferWriter& writer,
                             JSScript** scriptList, uint32_t scriptListSize,
                             uint32_t runLength, const CodeGeneratorShared::NativeToBytecode* entry)
{
    MOZ_ASSERT(runLength > 0);
    MOZ_ASSERT(runLength <= MAX_RUN_LENGTH);

    // Calculate script depth.
    MOZ_ASSERT(entry->tree->depth() <= 0xff);
    uint8_t scriptDepth = entry->tree->depth();
    uint32_t regionNativeOffset = entry->nativeOffset.offset();

    JitcodeMapBufferWriteSpewer spewer(writer);

    // Write the head info.
    JitSpew(JitSpew_Profiling, "    Head Info: nativeOffset=%d scriptDepth=%d",
            int(regionNativeOffset), int(scriptDepth));
    WriteHead(writer, regionNativeOffset, scriptDepth);
    spewer.spewAndAdvance("      ");

    // Write each script/pc pair.
    {
        InlineScriptTree* curTree = entry->tree;
        jsbytecode* curPc = entry->pc;
        for (uint8_t i = 0; i < scriptDepth; i++) {
            // Find the index of the script within the list.
            // NB: scriptList is guaranteed to contain curTree->script()
            uint32_t scriptIdx = 0;
            for (; scriptIdx < scriptListSize; scriptIdx++) {
                if (scriptList[scriptIdx] == curTree->script())
                    break;
            }
            MOZ_ASSERT(scriptIdx < scriptListSize);

            uint32_t pcOffset = curTree->script()->pcToOffset(curPc);

            JitSpew(JitSpew_Profiling, "    Script/PC %d: scriptIdx=%d pcOffset=%d",
                    int(i), int(scriptIdx), int(pcOffset));
            WriteScriptPc(writer, scriptIdx, pcOffset);
            spewer.spewAndAdvance("      ");

            MOZ_ASSERT_IF(i < scriptDepth - 1, curTree->hasCaller());
            curPc = curTree->callerPc();
            curTree = curTree->caller();
        }
    }

    // Start writing runs.
    uint32_t curNativeOffset = entry->nativeOffset.offset();
    uint32_t curBytecodeOffset = entry->tree->script()->pcToOffset(entry->pc);

    JitSpew(JitSpew_Profiling, "  Writing Delta Run from nativeOffset=%d bytecodeOffset=%d",
            int(curNativeOffset), int(curBytecodeOffset));

    // Skip first entry because it is implicit in the header.  Start at subsequent entry.
    for (uint32_t i = 1; i < runLength; i++) {
        MOZ_ASSERT(entry[i].tree == entry->tree);

        uint32_t nextNativeOffset = entry[i].nativeOffset.offset();
        uint32_t nextBytecodeOffset = entry[i].tree->script()->pcToOffset(entry[i].pc);
        MOZ_ASSERT(nextNativeOffset >= curNativeOffset);

        uint32_t nativeDelta = nextNativeOffset - curNativeOffset;
        int32_t bytecodeDelta = int32_t(nextBytecodeOffset) - int32_t(curBytecodeOffset);
        MOZ_ASSERT(IsDeltaEncodeable(nativeDelta, bytecodeDelta));

        JitSpew(JitSpew_Profiling, "    RunEntry native: %d-%d [%d]  bytecode: %d-%d [%d]",
                int(curNativeOffset), int(nextNativeOffset), int(nativeDelta),
                int(curBytecodeOffset), int(nextBytecodeOffset), int(bytecodeDelta));
        WriteDelta(writer, nativeDelta, bytecodeDelta);

        // Spew the bytecode in these ranges.
        if (curBytecodeOffset < nextBytecodeOffset) {
            JitSpewStart(JitSpew_Profiling, "      OPS: ");
            uint32_t curBc = curBytecodeOffset;
            while (curBc < nextBytecodeOffset) {
                jsbytecode* pc = entry[i].tree->script()->offsetToPC(curBc);
                JSOp op = JSOp(*pc);
                JitSpewCont(JitSpew_Profiling, "%s ", js_CodeName[op]);
                curBc += GetBytecodeLength(pc);
            }
            JitSpewFin(JitSpew_Profiling);
        }
        spewer.spewAndAdvance("      ");

        curNativeOffset = nextNativeOffset;
        curBytecodeOffset = nextBytecodeOffset;
    }

    if (writer.oom())
        return false;

    return true;
}

void
JitcodeRegionEntry::unpack()
{
    CompactBufferReader reader(data_, end_);
    ReadHead(reader, &nativeOffset_, &scriptDepth_);
    MOZ_ASSERT(scriptDepth_ > 0);

    scriptPcStack_ = reader.currentPosition();
    // Skip past script/pc stack
    for (unsigned i = 0; i < scriptDepth_; i++) {
        uint32_t scriptIdx, pcOffset;
        ReadScriptPc(reader, &scriptIdx, &pcOffset);
    }

    deltaRun_ = reader.currentPosition();
}

uint32_t
JitcodeRegionEntry::findPcOffset(uint32_t queryNativeOffset, uint32_t startPcOffset) const
{
    DeltaIterator iter = deltaIterator();
    uint32_t curNativeOffset = nativeOffset();
    uint32_t curPcOffset = startPcOffset;
    while (iter.hasMore()) {
        uint32_t nativeDelta;
        int32_t pcDelta;
        iter.readNext(&nativeDelta, &pcDelta);

        // The start address of the next delta-run entry is counted towards
        // the current delta-run entry, because return addresses should
        // associate with the bytecode op prior (the call) not the op after.
        if (queryNativeOffset <= curNativeOffset + nativeDelta)
            break;
        curNativeOffset += nativeDelta;
        curPcOffset += pcDelta;
    }
    return curPcOffset;
}

typedef js::Vector<char*, 32, SystemAllocPolicy> ProfilingStringVector;

struct AutoFreeProfilingStrings {
    ProfilingStringVector& profilingStrings_;
    bool keep_;
    explicit AutoFreeProfilingStrings(ProfilingStringVector& vec)
        : profilingStrings_(vec),
          keep_(false)
    {}

    void keepStrings() { keep_ = true; }

    ~AutoFreeProfilingStrings() {
        if (keep_)
            return;
        for (size_t i = 0; i < profilingStrings_.length(); i++)
            js_free(profilingStrings_[i]);
    }
};

bool
JitcodeIonTable::makeIonEntry(JSContext* cx, JitCode* code,
                              uint32_t numScripts, JSScript** scripts,
                              JitcodeGlobalEntry::IonEntry& out)
{
    typedef JitcodeGlobalEntry::IonEntry::SizedScriptList SizedScriptList;

    MOZ_ASSERT(numScripts > 0);

    // Create profiling strings for script, within vector.
    typedef js::Vector<char*, 32, SystemAllocPolicy> ProfilingStringVector;

    ProfilingStringVector profilingStrings;
    if (!profilingStrings.reserve(numScripts))
        return false;

    AutoFreeProfilingStrings autoFreeProfilingStrings(profilingStrings);
    for (uint32_t i = 0; i < numScripts; i++) {
        char* str = JitcodeGlobalEntry::createScriptString(cx, scripts[i]);
        if (!str)
            return false;
        if (!profilingStrings.append(str))
            return false;
    }

    // Create SizedScriptList
    void* mem = (void*)cx->pod_malloc<uint8_t>(SizedScriptList::AllocSizeFor(numScripts));
    if (!mem)
        return false;

    // Keep allocated profiling strings on destruct.
    autoFreeProfilingStrings.keepStrings();

    SizedScriptList* scriptList = new (mem) SizedScriptList(numScripts, scripts,
                                                            &profilingStrings[0]);
    out.init(code->raw(), code->rawEnd(), scriptList, this);
    return true;
}

uint32_t
JitcodeIonTable::findRegionEntry(uint32_t nativeOffset) const
{
    static const uint32_t LINEAR_SEARCH_THRESHOLD = 8;
    uint32_t regions = numRegions();
    MOZ_ASSERT(regions > 0);

    // For small region lists, just search linearly.
    if (regions <= LINEAR_SEARCH_THRESHOLD) {
        JitcodeRegionEntry previousEntry = regionEntry(0);
        for (uint32_t i = 1; i < regions; i++) {
            JitcodeRegionEntry nextEntry = regionEntry(i);
            MOZ_ASSERT(nextEntry.nativeOffset() >= previousEntry.nativeOffset());

            // See note in binary-search code below about why we use '<=' here instead of
            // '<'.  Short explanation: regions are closed at their ending addresses,
            // and open at their starting addresses.
            if (nativeOffset <= nextEntry.nativeOffset())
                return i-1;

            previousEntry = nextEntry;
        }
        // If nothing found, assume it falls within last region.
        return regions - 1;
    }

    // For larger ones, binary search the region table.
    uint32_t idx = 0;
    uint32_t count = regions;
    while (count > 1) {
        uint32_t step = count/2;
        uint32_t mid = idx + step;
        JitcodeRegionEntry midEntry = regionEntry(mid);

        // A region memory range is closed at its ending address, not starting
        // address.  This is because the return address for calls must associate
        // with the call's bytecode PC, not the PC of the bytecode operator after
        // the call.
        //
        // So a query is < an entry if the query nativeOffset is <= the start address
        // of the entry, and a query is >= an entry if the query nativeOffset is > the
        // start address of an entry.
        if (nativeOffset <= midEntry.nativeOffset()) {
            // Target entry is below midEntry.
            count = step;
        } else { // if (nativeOffset > midEntry.nativeOffset())
            // Target entry is at midEntry or above.
            idx = mid;
            count -= step;
        }
    }
    return idx;
}

/* static */ bool
JitcodeIonTable::WriteIonTable(CompactBufferWriter& writer,
                               JSScript** scriptList, uint32_t scriptListSize,
                               const CodeGeneratorShared::NativeToBytecode* start,
                               const CodeGeneratorShared::NativeToBytecode* end,
                               uint32_t* tableOffsetOut, uint32_t* numRegionsOut)
{
    MOZ_ASSERT(tableOffsetOut != nullptr);
    MOZ_ASSERT(numRegionsOut != nullptr);
    MOZ_ASSERT(writer.length() == 0);
    MOZ_ASSERT(scriptListSize > 0);

    JitSpew(JitSpew_Profiling, "Writing native to bytecode map for %s:%d (%d entries)",
            scriptList[0]->filename(), scriptList[0]->lineno(),
            int(end - start));

    JitSpew(JitSpew_Profiling, "  ScriptList of size %d", int(scriptListSize));
    for (uint32_t i = 0; i < scriptListSize; i++) {
        JitSpew(JitSpew_Profiling, "  Script %d - %s:%d",
                int(i), scriptList[i]->filename(), int(scriptList[i]->lineno()));
    }

    // Write out runs first.  Keep a vector tracking the positive offsets from payload
    // start to the run.
    const CodeGeneratorShared::NativeToBytecode* curEntry = start;
    js::Vector<uint32_t, 32, SystemAllocPolicy> runOffsets;

    while (curEntry != end) {
        // Calculate the length of the next run.
        uint32_t runLength = JitcodeRegionEntry::ExpectedRunLength(curEntry, end);
        MOZ_ASSERT(runLength > 0);
        MOZ_ASSERT(runLength <= uintptr_t(end - curEntry));
        JitSpew(JitSpew_Profiling, "  Run at entry %d, length %d, buffer offset %d",
                int(curEntry - start), int(runLength), int(writer.length()));

        // Store the offset of the run.
        if (!runOffsets.append(writer.length()))
            return false;

        // Encode the run.
        if (!JitcodeRegionEntry::WriteRun(writer, scriptList, scriptListSize, runLength, curEntry))
            return false;

        curEntry += runLength;
    }

    // Done encoding regions.  About to start table.  Ensure we are aligned to 4 bytes
    // since table is composed of uint32_t values.
    uint32_t padding = sizeof(uint32_t) - (writer.length() % sizeof(uint32_t));
    if (padding == sizeof(uint32_t))
        padding = 0;
    JitSpew(JitSpew_Profiling, "  Padding %d bytes after run @%d",
            int(padding), int(writer.length()));
    for (uint32_t i = 0; i < padding; i++)
        writer.writeByte(0);

    // Now at start of table.
    uint32_t tableOffset = writer.length();

    // The table being written at this point will be accessed directly via uint32_t
    // pointers, so all writes below use native endianness.

    // Write out numRegions
    JitSpew(JitSpew_Profiling, "  Writing numRuns=%d", int(runOffsets.length()));
    writer.writeNativeEndianUint32_t(runOffsets.length());

    // Write out region offset table.  The offsets in |runOffsets| are currently forward
    // offsets from the beginning of the buffer.  We convert them to backwards offsets
    // from the start of the table before writing them into their table entries.
    for (uint32_t i = 0; i < runOffsets.length(); i++) {
        JitSpew(JitSpew_Profiling, "  Run %d offset=%d backOffset=%d @%d",
                int(i), int(runOffsets[i]), int(tableOffset - runOffsets[i]), int(writer.length()));
        writer.writeNativeEndianUint32_t(tableOffset - runOffsets[i]);
    }

    if (writer.oom())
        return false;

    *tableOffsetOut = tableOffset;
    *numRegionsOut = runOffsets.length();
    return true;
}


} // namespace jit
} // namespace js
