/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
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

#include "debugger/Debugger.h"
#include "debugger/Environment.h"
#include "debugger/Frame.h"
#include "debugger/Script.h"
#include "debugger/Source.h"
#include "ds/Sort.h"
#include "jit/AutoWritableJitCode.h"
#include "jit/ExecutableAllocator.h"
#include "jit/MacroAssembler.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmJS.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"

#include "gc/FreeOp-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::BinarySearchIf;

DebugState::DebugState(const Code& code, const Module& module)
    : code_(&code),
      module_(&module),
      enterFrameTrapsEnabled_(false),
      enterAndLeaveFrameTrapsCounter_(0) {
  MOZ_ASSERT(code.metadata().debugEnabled);
}

void DebugState::trace(JSTracer* trc) {
  for (auto iter = breakpointSites_.iter(); !iter.done(); iter.next()) {
    WasmBreakpointSite* site = iter.get().value();
    site->trace(trc);
  }
}

void DebugState::finalize(JSFreeOp* fop) {
  for (auto iter = breakpointSites_.iter(); !iter.done(); iter.next()) {
    WasmBreakpointSite* site = iter.get().value();
    site->delete_(fop);
  }
}

static const uint32_t DefaultBinarySourceColumnNumber = 1;

static const CallSite* SlowCallSiteSearchByOffset(const MetadataTier& metadata,
                                                  uint32_t offset) {
  for (const CallSite& callSite : metadata.callSites) {
    if (callSite.lineOrBytecode() == offset &&
        callSite.kind() == CallSiteDesc::Breakpoint) {
      return &callSite;
    }
  }
  return nullptr;
}

bool DebugState::getLineOffsets(size_t lineno, Vector<uint32_t>* offsets) {
  const CallSite* callsite =
      SlowCallSiteSearchByOffset(metadata(Tier::Debug), lineno);
  return !(callsite && !offsets->append(lineno));
}

bool DebugState::getAllColumnOffsets(Vector<ExprLoc>* offsets) {
  for (const CallSite& callSite : metadata(Tier::Debug).callSites) {
    if (callSite.kind() != CallSite::Breakpoint) {
      continue;
    }
    uint32_t offset = callSite.lineOrBytecode();
    if (!offsets->emplaceBack(offset, DefaultBinarySourceColumnNumber,
                              offset)) {
      return false;
    }
  }
  return true;
}

bool DebugState::getOffsetLocation(uint32_t offset, size_t* lineno,
                                   size_t* column) {
  if (!SlowCallSiteSearchByOffset(metadata(Tier::Debug), offset)) {
    return false;
  }
  *lineno = offset;
  *column = DefaultBinarySourceColumnNumber;
  return true;
}

bool DebugState::stepModeEnabled(uint32_t funcIndex) const {
  return stepperCounters_.lookup(funcIndex).found();
}

bool DebugState::incrementStepperCount(JSContext* cx, uint32_t funcIndex) {
  const CodeRange& codeRange =
      codeRanges(Tier::Debug)[funcToCodeRangeIndex(funcIndex)];
  MOZ_ASSERT(codeRange.isFunction());

  StepperCounters::AddPtr p = stepperCounters_.lookupForAdd(funcIndex);
  if (p) {
    MOZ_ASSERT(p->value() > 0);
    p->value()++;
    return true;
  }
  if (!stepperCounters_.add(p, funcIndex, 1)) {
    ReportOutOfMemory(cx);
    return false;
  }

  AutoWritableJitCode awjc(
      cx->runtime(), code_->segment(Tier::Debug).base() + codeRange.begin(),
      codeRange.end() - codeRange.begin());

  for (const CallSite& callSite : callSites(Tier::Debug)) {
    if (callSite.kind() != CallSite::Breakpoint) {
      continue;
    }
    uint32_t offset = callSite.returnAddressOffset();
    if (codeRange.begin() <= offset && offset <= codeRange.end()) {
      toggleDebugTrap(offset, true);
    }
  }
  return true;
}

void DebugState::decrementStepperCount(JSFreeOp* fop, uint32_t funcIndex) {
  const CodeRange& codeRange =
      codeRanges(Tier::Debug)[funcToCodeRangeIndex(funcIndex)];
  MOZ_ASSERT(codeRange.isFunction());

  MOZ_ASSERT(!stepperCounters_.empty());
  StepperCounters::Ptr p = stepperCounters_.lookup(funcIndex);
  MOZ_ASSERT(p);
  if (--p->value()) {
    return;
  }

  stepperCounters_.remove(p);

  AutoWritableJitCode awjc(
      fop->runtime(), code_->segment(Tier::Debug).base() + codeRange.begin(),
      codeRange.end() - codeRange.begin());

  for (const CallSite& callSite : callSites(Tier::Debug)) {
    if (callSite.kind() != CallSite::Breakpoint) {
      continue;
    }
    uint32_t offset = callSite.returnAddressOffset();
    if (codeRange.begin() <= offset && offset <= codeRange.end()) {
      bool enabled = breakpointSites_.has(offset);
      toggleDebugTrap(offset, enabled);
    }
  }
}

bool DebugState::hasBreakpointTrapAtOffset(uint32_t offset) {
  return SlowCallSiteSearchByOffset(metadata(Tier::Debug), offset);
}

void DebugState::toggleBreakpointTrap(JSRuntime* rt, uint32_t offset,
                                      bool enabled) {
  const CallSite* callSite =
      SlowCallSiteSearchByOffset(metadata(Tier::Debug), offset);
  if (!callSite) {
    return;
  }
  size_t debugTrapOffset = callSite->returnAddressOffset();

  const ModuleSegment& codeSegment = code_->segment(Tier::Debug);
  const CodeRange* codeRange =
      code_->lookupFuncRange(codeSegment.base() + debugTrapOffset);
  MOZ_ASSERT(codeRange);

  if (stepperCounters_.lookup(codeRange->funcIndex())) {
    return;  // no need to toggle when step mode is enabled
  }

  AutoWritableJitCode awjc(rt, codeSegment.base(), codeSegment.length());
  toggleDebugTrap(debugTrapOffset, enabled);
}

WasmBreakpointSite* DebugState::getBreakpointSite(uint32_t offset) const {
  WasmBreakpointSiteMap::Ptr p = breakpointSites_.lookup(offset);
  if (!p) {
    return nullptr;
  }

  return p->value();
}

WasmBreakpointSite* DebugState::getOrCreateBreakpointSite(JSContext* cx,
                                                          Instance* instance,
                                                          uint32_t offset) {
  WasmBreakpointSite* site;

  WasmBreakpointSiteMap::AddPtr p = breakpointSites_.lookupForAdd(offset);
  if (!p) {
    site = cx->new_<WasmBreakpointSite>(instance->object(), offset);
    if (!site) {
      return nullptr;
    }

    if (!breakpointSites_.add(p, offset, site)) {
      js_delete(site);
      ReportOutOfMemory(cx);
      return nullptr;
    }

    AddCellMemory(instance->object(), sizeof(WasmBreakpointSite),
                  MemoryUse::BreakpointSite);

    toggleBreakpointTrap(cx->runtime(), offset, true);
  } else {
    site = p->value();
  }
  return site;
}

bool DebugState::hasBreakpointSite(uint32_t offset) {
  return breakpointSites_.has(offset);
}

void DebugState::destroyBreakpointSite(JSFreeOp* fop, Instance* instance,
                                       uint32_t offset) {
  WasmBreakpointSiteMap::Ptr p = breakpointSites_.lookup(offset);
  MOZ_ASSERT(p);
  fop->delete_(instance->objectUnbarriered(), p->value(),
               MemoryUse::BreakpointSite);
  breakpointSites_.remove(p);
  toggleBreakpointTrap(fop->runtime(), offset, false);
}

void DebugState::clearBreakpointsIn(JSFreeOp* fop, WasmInstanceObject* instance,
                                    js::Debugger* dbg, JSObject* handler) {
  MOZ_ASSERT(instance);

  // Breakpoints hold wrappers in the instance's compartment for the handler.
  // Make sure we don't try to search for the unwrapped handler.
  MOZ_ASSERT_IF(handler, instance->compartment() == handler->compartment());

  if (breakpointSites_.empty()) {
    return;
  }
  for (WasmBreakpointSiteMap::Enum e(breakpointSites_); !e.empty();
       e.popFront()) {
    WasmBreakpointSite* site = e.front().value();
    MOZ_ASSERT(site->instanceObject == instance);

    Breakpoint* nextbp;
    for (Breakpoint* bp = site->firstBreakpoint(); bp; bp = nextbp) {
      nextbp = bp->nextInSite();
      MOZ_ASSERT(bp->site == site);
      if ((!dbg || bp->debugger == dbg) &&
          (!handler || bp->getHandler() == handler)) {
        bp->delete_(fop);
      }
    }
    if (site->isEmpty()) {
      fop->delete_(instance, site, MemoryUse::BreakpointSite);
      e.removeFront();
    }
  }
}

void DebugState::toggleDebugTrap(uint32_t offset, bool enabled) {
  MOZ_ASSERT(offset);
  uint8_t* trap = code_->segment(Tier::Debug).base() + offset;
  const Uint32Vector& farJumpOffsets =
      metadata(Tier::Debug).debugTrapFarJumpOffsets;
  if (enabled) {
    MOZ_ASSERT(farJumpOffsets.length() > 0);
    size_t i = 0;
    while (i < farJumpOffsets.length() && offset < farJumpOffsets[i]) {
      i++;
    }
    if (i >= farJumpOffsets.length() ||
        (i > 0 &&
         offset - farJumpOffsets[i - 1] < farJumpOffsets[i] - offset)) {
      i--;
    }
    uint8_t* farJump = code_->segment(Tier::Debug).base() + farJumpOffsets[i];
    MacroAssembler::patchNopToCall(trap, farJump);
  } else {
    MacroAssembler::patchCallToNop(trap);
  }
}

void DebugState::adjustEnterAndLeaveFrameTrapsState(JSContext* cx,
                                                    bool enabled) {
  MOZ_ASSERT_IF(!enabled, enterAndLeaveFrameTrapsCounter_ > 0);

  bool wasEnabled = enterAndLeaveFrameTrapsCounter_ > 0;
  if (enabled) {
    ++enterAndLeaveFrameTrapsCounter_;
  } else {
    --enterAndLeaveFrameTrapsCounter_;
  }
  bool stillEnabled = enterAndLeaveFrameTrapsCounter_ > 0;
  if (wasEnabled == stillEnabled) {
    return;
  }

  const ModuleSegment& codeSegment = code_->segment(Tier::Debug);
  AutoWritableJitCode awjc(cx->runtime(), codeSegment.base(),
                           codeSegment.length());
  for (const CallSite& callSite : callSites(Tier::Debug)) {
    if (callSite.kind() != CallSite::EnterFrame &&
        callSite.kind() != CallSite::LeaveFrame) {
      continue;
    }
    toggleDebugTrap(callSite.returnAddressOffset(), stillEnabled);
  }
}

void DebugState::ensureEnterFrameTrapsState(JSContext* cx, bool enabled) {
  if (enterFrameTrapsEnabled_ == enabled) {
    return;
  }

  adjustEnterAndLeaveFrameTrapsState(cx, enabled);

  enterFrameTrapsEnabled_ = enabled;
}

bool DebugState::debugGetLocalTypes(uint32_t funcIndex, ValTypeVector* locals,
                                    size_t* argsLength,
                                    StackResults* stackResults) {
  const ValTypeVector& args = metadata().debugFuncArgTypes[funcIndex];
  const ValTypeVector& results = metadata().debugFuncReturnTypes[funcIndex];
  ResultType resultType(ResultType::Vector(results));
  *argsLength = args.length();
  *stackResults = ABIResultIter::HasStackResults(resultType)
                      ? StackResults::HasStackResults
                      : StackResults::NoStackResults;
  if (!locals->appendAll(args)) {
    return false;
  }

  // Decode local var types from wasm binary function body.
  const CodeRange& range =
      codeRanges(Tier::Debug)[funcToCodeRangeIndex(funcIndex)];
  // In wasm, the Code points to the function start via funcLineOrBytecode.
  size_t offsetInModule = range.funcLineOrBytecode();
  Decoder d(bytecode().begin() + offsetInModule, bytecode().end(),
            offsetInModule,
            /* error = */ nullptr);
  return DecodeValidatedLocalEntries(d, locals);
}

bool DebugState::getGlobal(Instance& instance, uint32_t globalIndex,
                           MutableHandleValue vp) {
  const GlobalDesc& global = metadata().globals[globalIndex];

  if (global.isConstant()) {
    LitVal value = global.constantValue();
    switch (value.type().kind()) {
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
      case ValType::Ref:
        // It's possible to do better.  We could try some kind of hashing
        // scheme, to make the pointer recognizable without revealing it.
        vp.set(MagicValue(JS_OPTIMIZED_OUT));
        break;
      case ValType::V128:
        // Debugger must be updated to handle this, and should be updated to
        // handle i64 in any case.
        vp.set(MagicValue(JS_OPTIMIZED_OUT));
        break;
      default:
        MOZ_CRASH("Global constant type");
    }
    return true;
  }

  uint8_t* globalData = instance.globalData();
  void* dataPtr = globalData + global.offset();
  if (global.isIndirect()) {
    dataPtr = *static_cast<void**>(dataPtr);
  }
  switch (global.type().kind()) {
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
    case ValType::Ref: {
      // Just hide it.  See above.
      vp.set(MagicValue(JS_OPTIMIZED_OUT));
      break;
    }
    case ValType::V128: {
      // Just hide it.  See above.
      vp.set(MagicValue(JS_OPTIMIZED_OUT));
      break;
    }
    default: {
      MOZ_CRASH("Global variable type");
      break;
    }
  }
  return true;
}

bool DebugState::getSourceMappingURL(JSContext* cx,
                                     MutableHandleString result) const {
  result.set(nullptr);

  for (const CustomSection& customSection : module_->customSections()) {
    const Bytes& sectionName = customSection.name;
    if (strlen(SourceMappingURLSectionName) != sectionName.length() ||
        memcmp(SourceMappingURLSectionName, sectionName.begin(),
               sectionName.length()) != 0) {
      continue;
    }

    // Parse found "SourceMappingURL" custom section.
    Decoder d(customSection.payload->begin(), customSection.payload->end(), 0,
              /* error = */ nullptr);
    uint32_t nchars;
    if (!d.readVarU32(&nchars)) {
      return true;  // ignoring invalid section data
    }
    const uint8_t* chars;
    if (!d.readBytes(nchars, &chars) || d.currentPosition() != d.end()) {
      return true;  // ignoring invalid section data
    }

    JS::UTF8Chars utf8Chars(reinterpret_cast<const char*>(chars), nchars);
    JSString* str = JS_NewStringCopyUTF8N(cx, utf8Chars);
    if (!str) {
      return false;
    }
    result.set(str);
    return true;
  }

  // Check presence of "SourceMap:" HTTP response header.
  char* sourceMapURL = metadata().sourceMapURL.get();
  if (sourceMapURL && strlen(sourceMapURL)) {
    JS::UTF8Chars utf8Chars(sourceMapURL, strlen(sourceMapURL));
    JSString* str = JS_NewStringCopyUTF8N(cx, utf8Chars);
    if (!str) {
      return false;
    }
    result.set(str);
  }
  return true;
}

void DebugState::addSizeOfMisc(MallocSizeOf mallocSizeOf,
                               Metadata::SeenSet* seenMetadata,
                               Code::SeenSet* seenCode, size_t* code,
                               size_t* data) const {
  code_->addSizeOfMiscIfNotSeen(mallocSizeOf, seenMetadata, seenCode, code,
                                data);
  module_->addSizeOfMisc(mallocSizeOf, seenMetadata, seenCode, code, data);
}
