/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_PerfSpewer_h
#define jit_PerfSpewer_h

#ifdef JS_ION_PERF
# include <stdio.h>
# include "jit/MacroAssembler.h"
#endif

namespace {
    struct AutoLockPerfMap;
}

namespace js {
namespace jit {

class MBasicBlock;
class MacroAssembler;

#ifdef JS_ION_PERF
void CheckPerf();
bool PerfBlockEnabled();
bool PerfFuncEnabled();
static inline bool PerfEnabled() {
    return PerfBlockEnabled() || PerfFuncEnabled();
}
#else
static inline void CheckPerf() {}
static inline bool PerfBlockEnabled() { return false; }
static inline bool PerfFuncEnabled() { return false; }
static inline bool PerfEnabled() { return false; }
#endif

#ifdef JS_ION_PERF

struct Record {
    const char* filename;
    unsigned lineNumber;
    unsigned columnNumber;
    uint32_t id;
    Label start, end;
    size_t startOffset, endOffset;

    Record(const char* filename,
           unsigned lineNumber,
           unsigned columnNumber,
           uint32_t id)
      : filename(filename), lineNumber(lineNumber),
        columnNumber(columnNumber), id(id),
        startOffset(0u), endOffset(0u)
    {}
};

typedef Vector<Record, 1, SystemAllocPolicy> BasicBlocksVector;

class PerfSpewer
{
  protected:
    static uint32_t nextFunctionIndex;

  public:
    Label endInlineCode;

  protected:
    BasicBlocksVector basicBlocks_;

  public:
    virtual MOZ_MUST_USE bool startBasicBlock(MBasicBlock* blk, MacroAssembler& masm);
    virtual void endBasicBlock(MacroAssembler& masm);
    void noteEndInlineCode(MacroAssembler& masm);

    void writeProfile(JSScript* script, JitCode* code, MacroAssembler& masm);

    static void WriteEntry(const AutoLockPerfMap&, uintptr_t address, size_t size,
                           const char* fmt, ...)
        MOZ_FORMAT_PRINTF(4, 5);
};

void writePerfSpewerBaselineProfile(JSScript* script, JitCode* code);
void writePerfSpewerJitCodeProfile(JitCode* code, const char* msg);

// wasm doesn't support block annotations.
class WasmPerfSpewer : public PerfSpewer
{
  public:
    MOZ_MUST_USE bool startBasicBlock(MBasicBlock* blk, MacroAssembler& masm) { return true; }
    void endBasicBlock(MacroAssembler& masm) { }
};

void writePerfSpewerWasmMap(uintptr_t base, uintptr_t size, const char* filename,
                            const char* annotation);
void writePerfSpewerWasmFunctionMap(uintptr_t base, uintptr_t size, const char* filename,
                                    unsigned lineno, const char* funcName);

#endif // JS_ION_PERF

} // namespace jit
} // namespace js

#endif /* jit_PerfSpewer_h */
