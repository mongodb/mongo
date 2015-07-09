/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_JitOptions_h
#define jit_JitOptions_h

#include "mozilla/Maybe.h"

#include "jit/IonTypes.h"
#include "js/TypeDecls.h"

namespace js {
namespace jit {

// Longer scripts can only be compiled off thread, as these compilations
// can be expensive and stall the main thread for too long.
static const uint32_t MAX_MAIN_THREAD_SCRIPT_SIZE = 2 * 1000;
static const uint32_t MAX_MAIN_THREAD_LOCALS_AND_ARGS = 256;

// Possible register allocators which may be used.
enum IonRegisterAllocator {
    RegisterAllocator_LSRA,
    RegisterAllocator_Backtracking,
    RegisterAllocator_Stupid
};

static inline mozilla::Maybe<IonRegisterAllocator>
LookupRegisterAllocator(const char* name)
{
    if (!strcmp(name, "lsra"))
        return mozilla::Some(RegisterAllocator_LSRA);
    if (!strcmp(name, "backtracking"))
        return mozilla::Some(RegisterAllocator_Backtracking);
    if (!strcmp(name, "stupid"))
        return mozilla::Some(RegisterAllocator_Stupid);
    return mozilla::Nothing();
}

struct JitOptions
{
    bool checkGraphConsistency;
#ifdef CHECK_OSIPOINT_REGISTERS
    bool checkOsiPointRegisters;
#endif
    bool checkRangeAnalysis;
    bool disableScalarReplacement;
    bool disableGvn;
    bool disableLicm;
    bool disableInlining;
    bool disableEdgeCaseAnalysis;
    bool disableRangeAnalysis;
    bool disableSink;
    bool disableLoopUnrolling;
    bool disableEaa;
    bool eagerCompilation;
    mozilla::Maybe<uint32_t> forcedDefaultIonWarmUpThreshold;
    mozilla::Maybe<IonRegisterAllocator> forcedRegisterAllocator;
    bool limitScriptSize;
    bool osr;
    uint32_t baselineWarmUpThreshold;
    uint32_t exceptionBailoutThreshold;
    uint32_t frequentBailoutThreshold;
    uint32_t maxStackArgs;
    uint32_t osrPcMismatchesBeforeRecompile;
    uint32_t smallFunctionMaxBytecodeLength_;

    JitOptions();
    bool isSmallFunction(JSScript* script) const;
    void setEagerCompilation();
    void setCompilerWarmUpThreshold(uint32_t warmUpThreshold);
    void resetCompilerWarmUpThreshold();
    void enableGvn(bool val);
};

extern JitOptions js_JitOptions;

} // namespace jit
} // namespace js

#endif /* jit_JitOptions_h */
