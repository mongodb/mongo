/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
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

#include "wasm/WasmDebug.h"

#include "mozilla/BinarySearch.h"

#include "ds/Sort.h"
#include "gc/FreeOp.h"
#include "jit/ExecutableAllocator.h"
#include "jit/MacroAssembler.h"
#include "util/StringBuffer.h"
#include "util/Text.h"
#include "vm/Debugger.h"
#include "wasm/WasmBinaryToText.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::BinarySearchIf;

bool
GeneratedSourceMap::searchLineByOffset(JSContext* cx, uint32_t offset, size_t* exprlocIndex)
{
    MOZ_ASSERT(!exprlocs_.empty());
    size_t exprlocsLength = exprlocs_.length();

    // Lazily build sorted array for fast log(n) lookup.
    if (!sortedByOffsetExprLocIndices_) {
        ExprLocIndexVector scratch;
        auto indices = MakeUnique<ExprLocIndexVector>();
        if (!indices || !indices->resize(exprlocsLength) || !scratch.resize(exprlocsLength)) {
            ReportOutOfMemory(cx);
            return false;
        }
        sortedByOffsetExprLocIndices_ = Move(indices);

        for (size_t i = 0; i < exprlocsLength; i++)
            (*sortedByOffsetExprLocIndices_)[i] = i;

        auto compareExprLocViaIndex = [&](uint32_t i, uint32_t j, bool* lessOrEqualp) -> bool {
            *lessOrEqualp = exprlocs_[i].offset <= exprlocs_[j].offset;
            return true;
        };
        MOZ_ALWAYS_TRUE(MergeSort(sortedByOffsetExprLocIndices_->begin(), exprlocsLength,
                                  scratch.begin(), compareExprLocViaIndex));
    }

    // Allowing non-exact search and if BinarySearchIf returns out-of-bound
    // index, moving the index to the last index.
    auto lookupFn = [&](uint32_t i) -> int {
        const ExprLoc& loc = exprlocs_[i];
        return offset == loc.offset ? 0 : offset < loc.offset ? -1 : 1;
    };
    size_t match;
    Unused << BinarySearchIf(sortedByOffsetExprLocIndices_->begin(), 0, exprlocsLength, lookupFn, &match);
    if (match >= exprlocsLength)
        match = exprlocsLength - 1;
    *exprlocIndex = (*sortedByOffsetExprLocIndices_)[match];
    return true;
}

size_t
GeneratedSourceMap::sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const
{
    size_t size = exprlocs_.sizeOfExcludingThis(mallocSizeOf);
    if (sortedByOffsetExprLocIndices_)
        size += sortedByOffsetExprLocIndices_->sizeOfIncludingThis(mallocSizeOf);
    return size;
}

DebugState::DebugState(SharedCode code,
                       const ShareableBytes* maybeBytecode,
                       bool binarySource)
  : code_(Move(code)),
    maybeBytecode_(maybeBytecode),
    binarySource_(binarySource),
    enterAndLeaveFrameTrapsCounter_(0)
{
    MOZ_ASSERT_IF(debugEnabled(), maybeBytecode);
}

const char enabledMessage[] =
    "Restart with developer tools open to view WebAssembly source";

const char tooBigMessage[] =
    "Unfortunately, this WebAssembly module is too big to view as text.\n"
    "We are working hard to remove this limitation.";

const char notGeneratedMessage[] =
    "WebAssembly text generation was disabled.";

static const unsigned TooBig = 1000000;

static const uint32_t DefaultBinarySourceColumnNumber = 1;

static const CallSite*
SlowCallSiteSearchByOffset(const MetadataTier& metadata, uint32_t offset)
{
    for (const CallSite& callSite : metadata.callSites) {
        if (callSite.lineOrBytecode() == offset && callSite.kind() == CallSiteDesc::Breakpoint)
            return &callSite;
    }
    return nullptr;
}

JSString*
DebugState::createText(JSContext* cx)
{
    StringBuffer buffer(cx);
    if (!maybeBytecode_) {
        if (!buffer.append(enabledMessage))
            return nullptr;

        MOZ_ASSERT(!maybeSourceMap_);
    } else if (binarySource_) {
        if (!buffer.append(notGeneratedMessage))
            return nullptr;
        return buffer.finishString();
    } else if (maybeBytecode_->bytes.length() > TooBig) {
        if (!buffer.append(tooBigMessage))
            return nullptr;

        MOZ_ASSERT(!maybeSourceMap_);
    } else {
        const Bytes& bytes = maybeBytecode_->bytes;
        auto sourceMap = MakeUnique<GeneratedSourceMap>();
        if (!sourceMap) {
            ReportOutOfMemory(cx);
            return nullptr;
        }
        maybeSourceMap_ = Move(sourceMap);

        if (!BinaryToText(cx, bytes.begin(), bytes.length(), buffer, maybeSourceMap_.get()))
            return nullptr;

#if DEBUG
        // Check that expression locations are sorted by line number.
        uint32_t lastLineno = 0;
        for (const ExprLoc& loc : maybeSourceMap_->exprlocs()) {
            MOZ_ASSERT(lastLineno <= loc.lineno);
            lastLineno = loc.lineno;
        }
#endif
    }

    return buffer.finishString();
}

bool
DebugState::ensureSourceMap(JSContext* cx)
{
    if (maybeSourceMap_ || !maybeBytecode_)
        return true;

    // We just need to cache maybeSourceMap_, ignoring the text result.
    return createText(cx);
}

struct LineComparator
{
    const uint32_t lineno;
    explicit LineComparator(uint32_t lineno) : lineno(lineno) {}

    int operator()(const ExprLoc& loc) const {
        return lineno == loc.lineno ? 0 : lineno < loc.lineno ? -1 : 1;
    }
};

bool
DebugState::getLineOffsets(JSContext* cx, size_t lineno, Vector<uint32_t>* offsets)
{
    if (!debugEnabled())
        return true;

    if (binarySource_) {
        const CallSite* callsite = SlowCallSiteSearchByOffset(metadata(Tier::Debug), lineno);
        if (callsite && !offsets->append(lineno))
            return false;
        return true;
    }

    if (!ensureSourceMap(cx))
        return false;

    if (!maybeSourceMap_)
        return true; // no source text available, keep offsets empty.

    ExprLocVector& exprlocs = maybeSourceMap_->exprlocs();

    // Binary search for the expression with the specified line number and
    // rewind to the first expression, if more than one expression on the same line.
    size_t match;
    if (!BinarySearchIf(exprlocs, 0, exprlocs.length(), LineComparator(lineno), &match))
        return true;

    while (match > 0 && exprlocs[match - 1].lineno == lineno)
        match--;

    // Return all expression offsets that were printed on the specified line.
    for (size_t i = match; i < exprlocs.length() && exprlocs[i].lineno == lineno; i++) {
        if (!offsets->append(exprlocs[i].offset))
            return false;
    }

    return true;
}

bool
DebugState::getAllColumnOffsets(JSContext* cx, Vector<ExprLoc>* offsets)
{
    if (!metadata().debugEnabled)
        return true;

    if (binarySource_) {
        for (const CallSite& callSite : metadata(Tier::Debug).callSites) {
            if (callSite.kind() != CallSite::Breakpoint)
                continue;
            uint32_t offset = callSite.lineOrBytecode();
            if (!offsets->emplaceBack(offset, DefaultBinarySourceColumnNumber, offset))
                return false;
        }
        return true;
    }

    if (!ensureSourceMap(cx))
        return false;

    if (!maybeSourceMap_)
        return true; // no source text available, keep offsets empty.

    return offsets->appendAll(maybeSourceMap_->exprlocs());
}

bool
DebugState::getOffsetLocation(JSContext* cx, uint32_t offset, bool* found, size_t* lineno, size_t* column)
{
    *found = false;
    if (!debugEnabled())
        return true;

    if (binarySource_) {
        if (!SlowCallSiteSearchByOffset(metadata(Tier::Debug), offset))
            return true; // offset was not found
        *found = true;
        *lineno = offset;
        *column = DefaultBinarySourceColumnNumber;
        return true;
    }

    if (!ensureSourceMap(cx))
        return false;

    if (!maybeSourceMap_ || maybeSourceMap_->exprlocs().empty())
        return true; // no source text available

    size_t foundAt;
    if (!maybeSourceMap_->searchLineByOffset(cx, offset, &foundAt))
        return false;

    const ExprLoc& loc = maybeSourceMap_->exprlocs()[foundAt];
    *found = true;
    *lineno = loc.lineno;
    *column = loc.column;
    return true;
}

bool
DebugState::totalSourceLines(JSContext* cx, uint32_t* count)
{
    *count = 0;
    if (!debugEnabled())
        return true;

    if (binarySource_) {
        if (maybeBytecode_)
            *count = maybeBytecode_->length();
        return true;
    }

    if (!ensureSourceMap(cx))
        return false;

    if (maybeSourceMap_)
        *count = maybeSourceMap_->totalLines();
    return true;
}

bool
DebugState::stepModeEnabled(uint32_t funcIndex) const
{
    return stepModeCounters_.initialized() && stepModeCounters_.lookup(funcIndex);
}

bool
DebugState::incrementStepModeCount(JSContext* cx, uint32_t funcIndex)
{
    MOZ_ASSERT(debugEnabled());
    const CodeRange& codeRange = codeRanges(Tier::Debug)[debugFuncToCodeRangeIndex(funcIndex)];
    MOZ_ASSERT(codeRange.isFunction());

    if (!stepModeCounters_.initialized() && !stepModeCounters_.init()) {
        ReportOutOfMemory(cx);
        return false;
    }

    StepModeCounters::AddPtr p = stepModeCounters_.lookupForAdd(funcIndex);
    if (p) {
        MOZ_ASSERT(p->value() > 0);
        p->value()++;
        return true;
    }
    if (!stepModeCounters_.add(p, funcIndex, 1)) {
        ReportOutOfMemory(cx);
        return false;
    }

    AutoWritableJitCode awjc(cx->runtime(), code_->segment(Tier::Debug).base() + codeRange.begin(),
                             codeRange.end() - codeRange.begin());
    AutoFlushICache afc("Code::incrementStepModeCount");

    for (const CallSite& callSite : callSites(Tier::Debug)) {
        if (callSite.kind() != CallSite::Breakpoint)
            continue;
        uint32_t offset = callSite.returnAddressOffset();
        if (codeRange.begin() <= offset && offset <= codeRange.end())
            toggleDebugTrap(offset, true);
    }
    return true;
}

bool
DebugState::decrementStepModeCount(FreeOp* fop, uint32_t funcIndex)
{
    MOZ_ASSERT(debugEnabled());
    const CodeRange& codeRange = codeRanges(Tier::Debug)[debugFuncToCodeRangeIndex(funcIndex)];
    MOZ_ASSERT(codeRange.isFunction());

    MOZ_ASSERT(stepModeCounters_.initialized() && !stepModeCounters_.empty());
    StepModeCounters::Ptr p = stepModeCounters_.lookup(funcIndex);
    MOZ_ASSERT(p);
    if (--p->value())
        return true;

    stepModeCounters_.remove(p);

    AutoWritableJitCode awjc(fop->runtime(), code_->segment(Tier::Debug).base() + codeRange.begin(),
                             codeRange.end() - codeRange.begin());
    AutoFlushICache afc("Code::decrementStepModeCount");

    for (const CallSite& callSite : callSites(Tier::Debug)) {
        if (callSite.kind() != CallSite::Breakpoint)
            continue;
        uint32_t offset = callSite.returnAddressOffset();
        if (codeRange.begin() <= offset && offset <= codeRange.end()) {
            bool enabled = breakpointSites_.initialized() && breakpointSites_.has(offset);
            toggleDebugTrap(offset, enabled);
        }
    }
    return true;
}

bool
DebugState::hasBreakpointTrapAtOffset(uint32_t offset)
{
    if (!debugEnabled())
        return false;
    return SlowCallSiteSearchByOffset(metadata(Tier::Debug), offset);
}

void
DebugState::toggleBreakpointTrap(JSRuntime* rt, uint32_t offset, bool enabled)
{
    MOZ_ASSERT(debugEnabled());
    const CallSite* callSite = SlowCallSiteSearchByOffset(metadata(Tier::Debug), offset);
    if (!callSite)
        return;
    size_t debugTrapOffset = callSite->returnAddressOffset();

    const ModuleSegment& codeSegment = code_->segment(Tier::Debug);
    const CodeRange* codeRange = code_->lookupFuncRange(codeSegment.base() + debugTrapOffset);
    MOZ_ASSERT(codeRange);

    if (stepModeCounters_.initialized() && stepModeCounters_.lookup(codeRange->funcIndex()))
        return; // no need to toggle when step mode is enabled

    AutoWritableJitCode awjc(rt, codeSegment.base(), codeSegment.length());
    AutoFlushICache afc("Code::toggleBreakpointTrap");
    AutoFlushICache::setRange(uintptr_t(codeSegment.base()), codeSegment.length());
    toggleDebugTrap(debugTrapOffset, enabled);
}

WasmBreakpointSite*
DebugState::getOrCreateBreakpointSite(JSContext* cx, uint32_t offset)
{
    WasmBreakpointSite* site;
    if (!breakpointSites_.initialized() && !breakpointSites_.init()) {
        ReportOutOfMemory(cx);
        return nullptr;
    }

    WasmBreakpointSiteMap::AddPtr p = breakpointSites_.lookupForAdd(offset);
    if (!p) {
        site = cx->zone()->new_<WasmBreakpointSite>(this, offset);
        if (!site || !breakpointSites_.add(p, offset, site)) {
            js_delete(site);
            ReportOutOfMemory(cx);
            return nullptr;
        }
    } else {
        site = p->value();
    }
    return site;
}

bool
DebugState::hasBreakpointSite(uint32_t offset)
{
    return breakpointSites_.initialized() && breakpointSites_.has(offset);
}

void
DebugState::destroyBreakpointSite(FreeOp* fop, uint32_t offset)
{
    MOZ_ASSERT(breakpointSites_.initialized());
    WasmBreakpointSiteMap::Ptr p = breakpointSites_.lookup(offset);
    MOZ_ASSERT(p);
    fop->delete_(p->value());
    breakpointSites_.remove(p);
}

bool
DebugState::clearBreakpointsIn(JSContext* cx, WasmInstanceObject* instance, js::Debugger* dbg, JSObject* handler)
{
    MOZ_ASSERT(instance);
    if (!breakpointSites_.initialized())
        return true;

    // Make copy of all sites list, so breakpointSites_ can be modified by
    // destroyBreakpointSite calls.
    Vector<WasmBreakpointSite*> sites(cx);
    if (!sites.resize(breakpointSites_.count()))
        return false;
    size_t i = 0;
    for (WasmBreakpointSiteMap::Range r = breakpointSites_.all(); !r.empty(); r.popFront())
        sites[i++] = r.front().value();

    for (WasmBreakpointSite* site : sites) {
        Breakpoint* nextbp;
        for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
            nextbp = bp->nextInSite();
            if (bp->asWasm()->wasmInstance == instance &&
                (!dbg || bp->debugger == dbg) &&
                (!handler || bp->getHandler() == handler))
            {
                bp->destroy(cx->runtime()->defaultFreeOp());
            }
        }
    }
    return true;
}

void
DebugState::toggleDebugTrap(uint32_t offset, bool enabled)
{
    MOZ_ASSERT(offset);
    uint8_t* trap = code_->segment(Tier::Debug).base() + offset;
    const Uint32Vector& farJumpOffsets = metadata(Tier::Debug).debugTrapFarJumpOffsets;
    if (enabled) {
        MOZ_ASSERT(farJumpOffsets.length() > 0);
        size_t i = 0;
        while (i < farJumpOffsets.length() && offset < farJumpOffsets[i])
            i++;
        if (i >= farJumpOffsets.length() ||
            (i > 0 && offset - farJumpOffsets[i - 1] < farJumpOffsets[i] - offset))
            i--;
        uint8_t* farJump = code_->segment(Tier::Debug).base() + farJumpOffsets[i];
        MacroAssembler::patchNopToCall(trap, farJump);
    } else {
        MacroAssembler::patchCallToNop(trap);
    }
}

void
DebugState::adjustEnterAndLeaveFrameTrapsState(JSContext* cx, bool enabled)
{
    MOZ_ASSERT(debugEnabled());
    MOZ_ASSERT_IF(!enabled, enterAndLeaveFrameTrapsCounter_ > 0);

    bool wasEnabled = enterAndLeaveFrameTrapsCounter_ > 0;
    if (enabled)
        ++enterAndLeaveFrameTrapsCounter_;
    else
        --enterAndLeaveFrameTrapsCounter_;
    bool stillEnabled = enterAndLeaveFrameTrapsCounter_ > 0;
    if (wasEnabled == stillEnabled)
        return;

    const ModuleSegment& codeSegment = code_->segment(Tier::Debug);
    AutoWritableJitCode awjc(cx->runtime(), codeSegment.base(), codeSegment.length());
    AutoFlushICache afc("Code::adjustEnterAndLeaveFrameTrapsState");
    AutoFlushICache::setRange(uintptr_t(codeSegment.base()), codeSegment.length());
    for (const CallSite& callSite : callSites(Tier::Debug)) {
        if (callSite.kind() != CallSite::EnterFrame && callSite.kind() != CallSite::LeaveFrame)
            continue;
        toggleDebugTrap(callSite.returnAddressOffset(), stillEnabled);
    }
}

bool
DebugState::debugGetLocalTypes(uint32_t funcIndex, ValTypeVector* locals, size_t* argsLength)
{
    MOZ_ASSERT(debugEnabled());

    const ValTypeVector& args = metadata().debugFuncArgTypes[funcIndex];
    *argsLength = args.length();
    if (!locals->appendAll(args))
        return false;

    // Decode local var types from wasm binary function body.
    const CodeRange& range = codeRanges(Tier::Debug)[debugFuncToCodeRangeIndex(funcIndex)];
    // In wasm, the Code points to the function start via funcLineOrBytecode.
    MOZ_ASSERT(!metadata().isAsmJS() && maybeBytecode_);
    size_t offsetInModule = range.funcLineOrBytecode();
    Decoder d(maybeBytecode_->begin() + offsetInModule,  maybeBytecode_->end(),
              offsetInModule, /* error = */ nullptr);
    return DecodeLocalEntries(d, metadata().kind, locals);
}

ExprType
DebugState::debugGetResultType(uint32_t funcIndex)
{
    MOZ_ASSERT(debugEnabled());
    return metadata().debugFuncReturnTypes[funcIndex];
}

bool
DebugState::getGlobal(Instance& instance, uint32_t globalIndex, MutableHandleValue vp)
{
    const GlobalDesc& global = metadata().globals[globalIndex];

    if (global.isConstant()) {
        Val value = global.constantValue();
        switch (value.type()) {
          case ValType::I32:
            vp.set(Int32Value(value.i32()));
            break;
          case ValType::I64:
          // Just display as a Number; it's ok if we lose some precision
            vp.set(NumberValue((double)value.i64()));
            break;
          case ValType::F32:
            vp.set(NumberValue(JS::CanonicalizeNaN(value.f32())));
            break;
          case ValType::F64:
            vp.set(NumberValue(JS::CanonicalizeNaN(value.f64())));
            break;
          default:
            MOZ_CRASH("Global constant type");
        }
        return true;
    }

    uint8_t* globalData = instance.globalData();
    void* dataPtr = globalData + global.offset();
    switch (global.type()) {
      case ValType::I32: {
        vp.set(Int32Value(*static_cast<int32_t*>(dataPtr)));
        break;
      }
      case ValType::I64: {
        // Just display as a Number; it's ok if we lose some precision
        vp.set(NumberValue((double)*static_cast<int64_t*>(dataPtr)));
        break;
      }
      case ValType::F32: {
        vp.set(NumberValue(JS::CanonicalizeNaN(*static_cast<float*>(dataPtr))));
        break;
      }
      case ValType::F64: {
        vp.set(NumberValue(JS::CanonicalizeNaN(*static_cast<double*>(dataPtr))));
        break;
      }
      default:
        MOZ_CRASH("Global variable type");
        break;
    }
    return true;
}


JSString*
DebugState::debugDisplayURL(JSContext* cx) const
{
    // Build wasm module URL from following parts:
    // - "wasm:" as protocol;
    // - URI encoded filename from metadata (if can be encoded), plus ":";
    // - 64-bit hash of the module bytes (as hex dump).

    js::StringBuffer result(cx);
    if (!result.append("wasm:"))
        return nullptr;

    if (const char* filename = metadata().filename.get()) {
        js::StringBuffer filenamePrefix(cx);
        // EncodeURI returns false due to invalid chars or OOM -- fail only
        // during OOM.
        if (!EncodeURI(cx, filenamePrefix, filename, strlen(filename))) {
            if (!cx->isExceptionPending())
                return nullptr;
            cx->clearPendingException(); // ignore invalid URI
        } else if (!result.append(filenamePrefix.finishString())) {
            return nullptr;
        }
    }

    if (metadata().debugEnabled) {
        if (!result.append(":"))
            return nullptr;

        const ModuleHash& hash = metadata().debugHash;
        for (size_t i = 0; i < sizeof(ModuleHash); i++) {
            char digit1 = hash[i] / 16, digit2 = hash[i] % 16;
            if (!result.append((char)(digit1 < 10 ? digit1 + '0' : digit1 + 'a' - 10)))
                return nullptr;
            if (!result.append((char)(digit2 < 10 ? digit2 + '0' : digit2 + 'a' - 10)))
                return nullptr;
        }
    }

    return result.finishString();
}

bool
DebugState::getSourceMappingURL(JSContext* cx, MutableHandleString result) const
{
    result.set(nullptr);
    if (!maybeBytecode_)
        return true;

    for (const CustomSection& customSection : metadata().customSections) {
        const NameInBytecode& sectionName = customSection.name;
        if (strlen(SourceMappingURLSectionName) != sectionName.length ||
            memcmp(SourceMappingURLSectionName, maybeBytecode_->begin() + sectionName.offset,
                   sectionName.length) != 0)
        {
            continue;
        }

        // Parse found "SourceMappingURL" custom section.
        Decoder d(maybeBytecode_->begin() + customSection.offset,
                  maybeBytecode_->begin() + customSection.offset + customSection.length,
                  customSection.offset,
                  /* error = */ nullptr);
        uint32_t nchars;
        if (!d.readVarU32(&nchars))
            return true; // ignoring invalid section data
        const uint8_t* chars;
        if (!d.readBytes(nchars, &chars) || d.currentPosition() != d.end())
            return true; // ignoring invalid section data

        UTF8Chars utf8Chars(reinterpret_cast<const char*>(chars), nchars);
        JSString* str = JS_NewStringCopyUTF8N(cx, utf8Chars);
        if (!str)
            return false;
        result.set(str);
        return true;
    }

    // Check presence of "SourceMap:" HTTP response header.
    char* sourceMapURL = metadata().sourceMapURL.get();
    if (sourceMapURL && strlen(sourceMapURL)) {
        UTF8Chars utf8Chars(sourceMapURL, strlen(sourceMapURL));
        JSString* str = JS_NewStringCopyUTF8N(cx, utf8Chars);
        if (!str)
            return false;
        result.set(str);
    }
    return true;
}

void
DebugState::addSizeOfMisc(MallocSizeOf mallocSizeOf,
                          Metadata::SeenSet* seenMetadata,
                          ShareableBytes::SeenSet* seenBytes,
                          Code::SeenSet* seenCode,
                          size_t* code,
                          size_t* data) const
{
    code_->addSizeOfMiscIfNotSeen(mallocSizeOf, seenMetadata, seenCode, code, data);
    if (maybeSourceMap_)
        *data += maybeSourceMap_->sizeOfExcludingThis(mallocSizeOf);
    if (maybeBytecode_)
        *data += maybeBytecode_->sizeOfIncludingThisIfNotSeen(mallocSizeOf, seenBytes);
}
