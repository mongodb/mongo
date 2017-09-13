/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2014 Mozilla Foundation
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

#ifndef asmjs_AsmJSFrameIterator_h
#define asmjs_AsmJSFrameIterator_h

#include <stdint.h>

#include "asmjs/Wasm.h"
#include "js/ProfilingFrameIterator.h"

class JSAtom;

namespace js {

class AsmJSActivation;
class AsmJSModule;
namespace jit { class MacroAssembler; class Label; }
namespace wasm { class CallSite; }

// Iterates over the frames of a single AsmJSActivation, called synchronously
// from C++ in the thread of the asm.js. The one exception is that this iterator
// may be called from the interrupt callback which may be called asynchronously
// from asm.js code; in this case, the backtrace may not be correct.
class AsmJSFrameIterator
{
    const AsmJSModule* module_;
    const wasm::CallSite* callsite_;
    uint8_t* fp_;

    // Really, a const AsmJSModule::CodeRange*, but no forward declarations of
    // nested classes, so use void* to avoid pulling in all of AsmJSModule.h.
    const void* codeRange_;

    void settle();

  public:
    explicit AsmJSFrameIterator() : module_(nullptr) {}
    explicit AsmJSFrameIterator(const AsmJSActivation& activation);
    void operator++();
    bool done() const { return !fp_; }
    JSAtom* functionDisplayAtom() const;
    unsigned computeLine(uint32_t* column) const;
};

// Iterates over the frames of a single AsmJSActivation, given an
// asynchrously-interrupted thread's state. If the activation's
// module is not in profiling mode, the activation is skipped.
class AsmJSProfilingFrameIterator
{
    const AsmJSModule* module_;
    uint8_t* callerFP_;
    void* callerPC_;
    void* stackAddress_;
    wasm::ExitReason exitReason_;

    // Really, a const AsmJSModule::CodeRange*, but no forward declarations of
    // nested classes, so use void* to avoid pulling in all of AsmJSModule.h.
    const void* codeRange_;

    void initFromFP(const AsmJSActivation& activation);

  public:
    AsmJSProfilingFrameIterator() : codeRange_(nullptr) {}
    explicit AsmJSProfilingFrameIterator(const AsmJSActivation& activation);
    AsmJSProfilingFrameIterator(const AsmJSActivation& activation,
                                const JS::ProfilingFrameIterator::RegisterState& state);
    void operator++();
    bool done() const { return !codeRange_; }

    void* stackAddress() const { MOZ_ASSERT(!done()); return stackAddress_; }
    const char* label() const;
};

/******************************************************************************/
// Prologue/epilogue code generation.

struct AsmJSOffsets
{
    MOZ_IMPLICIT AsmJSOffsets(uint32_t begin = 0,
                              uint32_t end = 0)
      : begin(begin), end(end)
    {}

    // These define a [begin, end) contiguous range of instructions compiled
    // into an AsmJSModule::CodeRange.
    uint32_t begin;
    uint32_t end;
};

struct AsmJSProfilingOffsets : AsmJSOffsets
{
    MOZ_IMPLICIT AsmJSProfilingOffsets(uint32_t profilingReturn = 0)
      : AsmJSOffsets(), profilingReturn(profilingReturn)
    {}

    // For CodeRanges with AsmJSProfilingOffsets, 'begin' is the offset of the
    // profiling entry.
    uint32_t profilingEntry() const { return begin; }

    // The profiling return is the offset of the return instruction, which
    // precedes the 'end' by a variable number of instructions due to
    // out-of-line codegen.
    uint32_t profilingReturn;
};

struct AsmJSFunctionOffsets : AsmJSProfilingOffsets
{
    MOZ_IMPLICIT AsmJSFunctionOffsets(uint32_t nonProfilingEntry = 0,
                                      uint32_t profilingJump = 0,
                                      uint32_t profilingEpilogue = 0)
      : AsmJSProfilingOffsets(),
        nonProfilingEntry(nonProfilingEntry),
        profilingJump(profilingJump),
        profilingEpilogue(profilingEpilogue)
    {}

    // Function CodeRanges have an additional non-profiling entry that comes
    // after the profiling entry and a non-profiling epilogue that comes before
    // the profiling epilogue.
    uint32_t nonProfilingEntry;

    // When profiling is enabled, the 'nop' at offset 'profilingJump' is
    // overwritten to be a jump to 'profilingEpilogue'.
    uint32_t profilingJump;
    uint32_t profilingEpilogue;
};

void
GenerateAsmJSExitPrologue(jit::MacroAssembler& masm, unsigned framePushed, wasm::ExitReason reason,
                          AsmJSProfilingOffsets* offsets, jit::Label* maybeEntry = nullptr);
void
GenerateAsmJSExitEpilogue(jit::MacroAssembler& masm, unsigned framePushed, wasm::ExitReason reason,
                          AsmJSProfilingOffsets* offsets);

void
GenerateAsmJSFunctionPrologue(jit::MacroAssembler& masm, unsigned framePushed,
                              AsmJSFunctionOffsets* offsets);
void
GenerateAsmJSFunctionEpilogue(jit::MacroAssembler& masm, unsigned framePushed,
                              AsmJSFunctionOffsets* offsets);

} // namespace js

#endif // asmjs_AsmJSFrameIterator_h
