/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/PerfSpewer.h"

#include "mozilla/IntegerPrintfMacros.h"

#ifdef XP_UNIX
# include <unistd.h>
#endif

#ifdef JS_ION_PERF
# include "jit/JitSpewer.h"
# include "jit/LIR.h"
# include "jit/MIR.h"
# include "jit/MIRGraph.h"
#endif

#include "vm/MutexIDs.h"

// perf expects its data to be in a file /tmp/perf-PID.map, but for Android
// and B2G the map files are written to /data/local/tmp/perf-PID.map
//
// Except that Android 4.3 no longer allows the browser to write to /data/local/tmp/
// so also try /sdcard/.

#ifndef PERF_SPEW_DIR
# if defined(__ANDROID__)
#  define PERF_SPEW_DIR "/data/local/tmp/"
#  define PERF_SPEW_DIR_2 "/sdcard/"
# else
#  define PERF_SPEW_DIR "/tmp/"
# endif
#endif

using namespace js;
using namespace js::jit;

#define PERF_MODE_NONE  1
#define PERF_MODE_FUNC  2
#define PERF_MODE_BLOCK 3

#ifdef JS_ION_PERF

static uint32_t PerfMode = 0;

static bool PerfChecked = false;

static FILE* PerfFilePtr = nullptr;

static js::Mutex* PerfMutex;

static bool
openPerfMap(const char* dir)
{
    const ssize_t bufferSize = 256;
    char filenameBuffer[bufferSize];

    if (snprintf(filenameBuffer, bufferSize, "%sperf-%d.map", dir, getpid()) >= bufferSize)
        return false;

    MOZ_ASSERT(!PerfFilePtr);
    PerfFilePtr = fopen(filenameBuffer, "a");

    if (!PerfFilePtr)
        return false;

    return true;
}

void
js::jit::CheckPerf() {
    if (!PerfChecked) {
        const char* env = getenv("IONPERF");
        if (env == nullptr) {
            PerfMode = PERF_MODE_NONE;
            fprintf(stderr, "Warning: JIT perf reporting requires IONPERF set to \"block\" or \"func\". ");
            fprintf(stderr, "Perf mapping will be deactivated.\n");
        } else if (!strcmp(env, "none")) {
            PerfMode = PERF_MODE_NONE;
        } else if (!strcmp(env, "block")) {
            PerfMode = PERF_MODE_BLOCK;
        } else if (!strcmp(env, "func")) {
            PerfMode = PERF_MODE_FUNC;
        } else {
            fprintf(stderr, "Use IONPERF=func to record at function granularity\n");
            fprintf(stderr, "Use IONPERF=block to record at basic block granularity\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Be advised that using IONPERF will cause all scripts\n");
            fprintf(stderr, "to be leaked.\n");
            exit(0);
        }

        if (PerfMode != PERF_MODE_NONE) {
            PerfMutex = js_new<js::Mutex>(mutexid::PerfSpewer);
            if (!PerfMutex)
                MOZ_CRASH("failed to allocate PerfMutex");

            if (openPerfMap(PERF_SPEW_DIR)) {
                PerfChecked = true;
                return;
            }

#if defined(__ANDROID__)
            if (openPerfMap(PERF_SPEW_DIR_2)) {
                PerfChecked = true;
                return;
            }
#endif
            fprintf(stderr, "Failed to open perf map file.  Disabling IONPERF.\n");
            PerfMode = PERF_MODE_NONE;
        }
        PerfChecked = true;
    }
}

bool
js::jit::PerfBlockEnabled() {
    MOZ_ASSERT(PerfMode);
    return PerfMode == PERF_MODE_BLOCK;
}

bool
js::jit::PerfFuncEnabled() {
    MOZ_ASSERT(PerfMode);
    return PerfMode == PERF_MODE_FUNC;
}

namespace {
    struct MOZ_RAII AutoLockPerfMap
    {
        AutoLockPerfMap() {
            if (!PerfEnabled())
                return;
            PerfMutex->lock();
            MOZ_ASSERT(PerfFilePtr);
        }
        ~AutoLockPerfMap() {
            MOZ_ASSERT(PerfFilePtr);
            fflush(PerfFilePtr);
            PerfMutex->unlock();
        }
    };
}

uint32_t PerfSpewer::nextFunctionIndex = 0;

bool
PerfSpewer::startBasicBlock(MBasicBlock* blk,
                            MacroAssembler& masm)
{
    if (!PerfBlockEnabled())
        return true;

    const char* filename = blk->info().script()->filename();
    unsigned lineNumber, columnNumber;
    if (blk->pc()) {
        lineNumber = PCToLineNumber(blk->info().script(),
                                    blk->pc(),
                                    &columnNumber);
    } else {
        lineNumber = 0;
        columnNumber = 0;
    }
    Record r(filename, lineNumber, columnNumber, blk->id());
    masm.bind(&r.start);
    return basicBlocks_.append(r);
}

void
PerfSpewer::endBasicBlock(MacroAssembler& masm)
{
    if (!PerfBlockEnabled())
        return;
    masm.bind(&basicBlocks_.back().end);
}

void
PerfSpewer::noteEndInlineCode(MacroAssembler& masm)
{
    if (!PerfBlockEnabled())
        return;
    masm.bind(&endInlineCode);
}

void
PerfSpewer::WriteEntry(const AutoLockPerfMap&, uintptr_t address, size_t size,
                       const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    auto result = mozilla::Vsmprintf<js::SystemAllocPolicy>(fmt, ap);
    va_end(ap);

    fprintf(PerfFilePtr, "%" PRIxPTR " %zx %s\n",
            address,
            size,
            result.get());
}

void
PerfSpewer::writeProfile(JSScript* script,
                         JitCode* code,
                         MacroAssembler& masm)
{
    AutoLockPerfMap lock;

    if (PerfFuncEnabled()) {
        uint32_t thisFunctionIndex = nextFunctionIndex++;
        size_t size = code->instructionsSize();
        if (size > 0) {
            WriteEntry(lock, reinterpret_cast<uintptr_t>(code->raw()), size, "%s:%zu: Func%02" PRIu32,
                       script->filename(), script->lineno(), thisFunctionIndex);
        }
        return;
    }

    if (PerfBlockEnabled() && basicBlocks_.length() > 0) {
        uint32_t thisFunctionIndex = nextFunctionIndex++;
        uintptr_t funcStart = uintptr_t(code->raw());
        uintptr_t funcEndInlineCode = funcStart + endInlineCode.offset();
        uintptr_t funcEnd = funcStart + code->instructionsSize();

        // function begins with the prologue, which is located before the first basic block
        size_t prologueSize = basicBlocks_[0].start.offset();

        if (prologueSize > 0) {
            WriteEntry(lock, funcStart, prologueSize, "%s:%zu: Func%02" PRIu32 "-Prologue",
                       script->filename(), script->lineno(), thisFunctionIndex);
        }

        uintptr_t cur = funcStart + prologueSize;
        for (uint32_t i = 0; i < basicBlocks_.length(); i++) {
            Record& r = basicBlocks_[i];

            uintptr_t blockStart = funcStart + r.start.offset();
            uintptr_t blockEnd = funcStart + r.end.offset();

            MOZ_ASSERT(cur <= blockStart);
            if (cur < blockStart) {
                WriteEntry(lock, cur, blockStart - cur, "%s:%zu: Func%02" PRIu32 "-Block?",
                           script->filename(), script->lineno(), thisFunctionIndex);
            }
            cur = blockEnd;

            size_t size = blockEnd - blockStart;

            if (size > 0) {
                WriteEntry(lock, blockStart, size, "%s:%u:%u: Func%02" PRIu32 "d-Block%" PRIu32,
                           r.filename, r.lineNumber, r.columnNumber, thisFunctionIndex, r.id);
            }
        }

        MOZ_ASSERT(cur <= funcEndInlineCode);
        if (cur < funcEndInlineCode) {
            WriteEntry(lock, cur, funcEndInlineCode - cur, "%s:%zu: Func%02" PRIu32 "-Epilogue",
                       script->filename(), script->lineno(), thisFunctionIndex);
        }

        MOZ_ASSERT(funcEndInlineCode <= funcEnd);
        if (funcEndInlineCode < funcEnd) {
            WriteEntry(lock, funcEndInlineCode, funcEnd - funcEndInlineCode,
                       "%s:%zu: Func%02" PRIu32 "-OOL",
                       script->filename(), script->lineno(), thisFunctionIndex);
        }
    }
}

void
js::jit::writePerfSpewerBaselineProfile(JSScript* script, JitCode* code)
{
    if (!PerfEnabled())
        return;

    size_t size = code->instructionsSize();
    if (size > 0) {
        AutoLockPerfMap lock;
        PerfSpewer::WriteEntry(lock, reinterpret_cast<uintptr_t>(code->raw()), size,
                               "%s:%zu: Baseline", script->filename(), script->lineno());
    }
}

void
js::jit::writePerfSpewerJitCodeProfile(JitCode* code, const char* msg)
{
    if (!code || !PerfEnabled())
        return;

    size_t size = code->instructionsSize();
    if (size > 0) {
        AutoLockPerfMap lock;
        PerfSpewer::WriteEntry(lock, reinterpret_cast<uintptr_t>(code->raw()), size,
                               "%s (%p 0x%zx)", msg, code->raw(), size);
    }
}

void
js::jit::writePerfSpewerWasmMap(uintptr_t base, uintptr_t size, const char* filename,
                                const char* annotation)
{
    if (!PerfFuncEnabled() || size == 0U)
        return;

    AutoLockPerfMap lock;
    PerfSpewer::WriteEntry(lock, base, size, "%s: Function %s", filename, annotation);
}

void
js::jit::writePerfSpewerWasmFunctionMap(uintptr_t base, uintptr_t size,
                                         const char* filename, unsigned lineno,
                                         const char* funcName)
{
    if (!PerfFuncEnabled() || size == 0U)
        return;

    AutoLockPerfMap lock;
    PerfSpewer::WriteEntry(lock, base, size, "%s:%u: Function %s", filename, lineno, funcName);
}

#endif // defined (JS_ION_PERF)
