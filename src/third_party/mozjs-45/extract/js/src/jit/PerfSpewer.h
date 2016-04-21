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
    virtual bool startBasicBlock(MBasicBlock* blk, MacroAssembler& masm);
    virtual bool endBasicBlock(MacroAssembler& masm);
    bool noteEndInlineCode(MacroAssembler& masm);

    void writeProfile(JSScript* script, JitCode* code, MacroAssembler& masm);
};

void writePerfSpewerBaselineProfile(JSScript* script, JitCode* code);
void writePerfSpewerJitCodeProfile(JitCode* code, const char* msg);

// AsmJS doesn't support block annotations.
class AsmJSPerfSpewer : public PerfSpewer
{
  public:
    bool startBasicBlock(MBasicBlock* blk, MacroAssembler& masm) { return true; }
    bool endBasicBlock(MacroAssembler& masm) { return true; }
};

void writePerfSpewerAsmJSFunctionMap(uintptr_t base, uintptr_t size, const char* filename,
                                     unsigned lineno, unsigned colIndex, const char* funcName);

#endif // JS_ION_PERF

} // namespace jit
} // namespace js

#endif /* jit_PerfSpewer_h */
