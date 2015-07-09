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

#include "js/ProfilingFrameIterator.h"

class JSAtom;
struct JSContext;

namespace js {

class AsmJSActivation;
class AsmJSModule;
struct AsmJSFunctionLabels;
namespace jit { class CallSite; class MacroAssembler; class Label; }

// Iterates over the frames of a single AsmJSActivation, called synchronously
// from C++ in the thread of the asm.js. The one exception is that this iterator
// may be called from the interrupt callback which may be called asynchronously
// from asm.js code; in this case, the backtrace may not be correct.
class AsmJSFrameIterator
{
    const AsmJSModule* module_;
    const jit::CallSite* callsite_;
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

namespace AsmJSExit
{
    // List of reasons for execution leaving asm.js-generated code, stored in
    // AsmJSActivation. The initial and default state is AsmJSNoExit. If
    // AsmJSNoExit is observed when the pc isn't in asm.js code, execution must
    // have been interrupted asynchronously (viz., by a exception/signal
    // handler).
    enum ReasonKind {
        Reason_None,
        Reason_JitFFI,
        Reason_SlowFFI,
        Reason_Interrupt,
        Reason_Builtin
    };

    // For Reason_Builtin, the list of builtins, so they can be displayed in the
    // profile call stack.
    enum BuiltinKind {
        Builtin_ToInt32,
#if defined(JS_CODEGEN_ARM)
        Builtin_IDivMod,
        Builtin_UDivMod,
#endif
        Builtin_ModD,
        Builtin_SinD,
        Builtin_CosD,
        Builtin_TanD,
        Builtin_ASinD,
        Builtin_ACosD,
        Builtin_ATanD,
        Builtin_CeilD,
        Builtin_CeilF,
        Builtin_FloorD,
        Builtin_FloorF,
        Builtin_ExpD,
        Builtin_LogD,
        Builtin_PowD,
        Builtin_ATan2D,
        Builtin_Limit
    };

    // A Reason contains both a ReasonKind and (if Reason_Builtin) a
    // BuiltinKind.
    typedef uint32_t Reason;

    static const uint32_t None = Reason_None;
    static const uint32_t JitFFI = Reason_JitFFI;
    static const uint32_t SlowFFI = Reason_SlowFFI;
    static const uint32_t Interrupt = Reason_Interrupt;
    static inline Reason Builtin(BuiltinKind builtin) {
        return uint16_t(Reason_Builtin) | (uint16_t(builtin) << 16);
    }
    static inline ReasonKind ExtractReasonKind(Reason reason) {
        return ReasonKind(uint16_t(reason));
    }
    static inline BuiltinKind ExtractBuiltinKind(Reason reason) {
        MOZ_ASSERT(ExtractReasonKind(reason) == Reason_Builtin);
        return BuiltinKind(uint16_t(reason >> 16));
    }
}

// Iterates over the frames of a single AsmJSActivation, given an
// asynchrously-interrupted thread's state. If the activation's
// module is not in profiling mode, the activation is skipped.
class AsmJSProfilingFrameIterator
{
    const AsmJSModule* module_;
    uint8_t* callerFP_;
    void* callerPC_;
    void* stackAddress_;
    AsmJSExit::Reason exitReason_;

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

void
GenerateAsmJSFunctionPrologue(jit::MacroAssembler& masm, unsigned framePushed,
                              AsmJSFunctionLabels* labels);
void
GenerateAsmJSFunctionEpilogue(jit::MacroAssembler& masm, unsigned framePushed,
                              AsmJSFunctionLabels* labels);
void
GenerateAsmJSStackOverflowExit(jit::MacroAssembler& masm, jit::Label* overflowExit,
                               jit::Label* throwLabel);

void
GenerateAsmJSExitPrologue(jit::MacroAssembler& masm, unsigned framePushed, AsmJSExit::Reason reason,
                          jit::Label* begin);
void
GenerateAsmJSExitEpilogue(jit::MacroAssembler& masm, unsigned framePushed, AsmJSExit::Reason reason,
                          jit::Label* profilingReturn);

} // namespace js

#endif // asmjs_AsmJSFrameIterator_h
