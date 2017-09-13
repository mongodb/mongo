/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_Ion_h
#define jit_Ion_h

#include "mozilla/MemoryReporting.h"

#include "jscntxt.h"
#include "jscompartment.h"

#include "jit/CompileWrappers.h"
#include "jit/JitOptions.h"

namespace js {
namespace jit {

class TempAllocator;

enum MethodStatus
{
    Method_Error,
    Method_CantCompile,
    Method_Skipped,
    Method_Compiled
};

enum AbortReason {
    AbortReason_Alloc,
    AbortReason_Inlining,
    AbortReason_PreliminaryObjects,
    AbortReason_Disable,
    AbortReason_Error,
    AbortReason_NoAbort
};

// A JIT context is needed to enter into either an JIT method or an instance
// of a JIT compiler. It points to a temporary allocator and the active
// JSContext, either of which may be nullptr, and the active compartment, which
// will not be nullptr.

class JitContext
{
  public:
    JitContext(JSContext* cx, TempAllocator* temp);
    JitContext(ExclusiveContext* cx, TempAllocator* temp);
    JitContext(CompileRuntime* rt, CompileCompartment* comp, TempAllocator* temp);
    explicit JitContext(CompileRuntime* rt);
    JitContext(CompileRuntime* rt, TempAllocator* temp);
    ~JitContext();

    // Running context when executing on the main thread. Not available during
    // compilation.
    JSContext* cx;

    // Allocator for temporary memory during compilation.
    TempAllocator* temp;

    // Wrappers with information about the current runtime/compartment for use
    // during compilation.
    CompileRuntime* runtime;
    CompileCompartment* compartment;

    int getNextAssemblerId() {
        return assemblerCount_++;
    }
  private:
    JitContext* prev_;
    int assemblerCount_;
};

// Initialize Ion statically for all JSRuntimes.
bool InitializeIon();

// Get and set the current JIT context.
JitContext* GetJitContext();
JitContext* MaybeGetJitContext();

void SetJitContext(JitContext* ctx);

bool CanIonCompileScript(JSContext* cx, JSScript* script, bool osr);

MethodStatus CanEnterAtBranch(JSContext* cx, HandleScript script,
                              BaselineFrame* frame, jsbytecode* pc);
MethodStatus CanEnter(JSContext* cx, RunState& state);
MethodStatus CompileFunctionForBaseline(JSContext* cx, HandleScript script, BaselineFrame* frame);
MethodStatus CanEnterUsingFastInvoke(JSContext* cx, HandleScript script, uint32_t numActualArgs);

MethodStatus
Recompile(JSContext* cx, HandleScript script, BaselineFrame* osrFrame, jsbytecode* osrPc,
          bool constructing, bool force);

enum JitExecStatus
{
    // The method call had to be aborted due to a stack limit check. This
    // error indicates that Ion never attempted to clean up frames.
    JitExec_Aborted,

    // The method call resulted in an error, and IonMonkey has cleaned up
    // frames.
    JitExec_Error,

    // The method call succeeded and returned a value.
    JitExec_Ok
};

static inline bool
IsErrorStatus(JitExecStatus status)
{
    return status == JitExec_Error || status == JitExec_Aborted;
}

struct EnterJitData;

bool SetEnterJitData(JSContext* cx, EnterJitData& data, RunState& state, AutoValueVector& vals);

JitExecStatus IonCannon(JSContext* cx, RunState& state);

// Used to enter Ion from C++ natives like Array.map. Called from FastInvokeGuard.
JitExecStatus FastInvoke(JSContext* cx, HandleFunction fun, CallArgs& args);

// Walk the stack and invalidate active Ion frames for the invalid scripts.
void Invalidate(TypeZone& types, FreeOp* fop,
                const RecompileInfoVector& invalid, bool resetUses = true,
                bool cancelOffThread = true);
void Invalidate(JSContext* cx, const RecompileInfoVector& invalid, bool resetUses = true,
                bool cancelOffThread = true);
bool Invalidate(JSContext* cx, JSScript* script, bool resetUses = true,
                bool cancelOffThread = true);

void ToggleBarriers(JS::Zone* zone, bool needs);

class IonBuilder;
class MIRGenerator;
class LIRGraph;
class CodeGenerator;

bool OptimizeMIR(MIRGenerator* mir);
LIRGraph* GenerateLIR(MIRGenerator* mir);
CodeGenerator* GenerateCode(MIRGenerator* mir, LIRGraph* lir);
CodeGenerator* CompileBackEnd(MIRGenerator* mir);

void AttachFinishedCompilations(JSContext* cx);
void FinishOffThreadBuilder(JSContext* cx, IonBuilder* builder);
void StopAllOffThreadCompilations(Zone* zone);
void StopAllOffThreadCompilations(JSCompartment* comp);

void LazyLink(JSContext* cx, HandleScript calleescript);
uint8_t* LazyLinkTopActivation(JSContext* cx);

static inline bool
IsIonEnabled(JSContext* cx)
{
    // The ARM64 Ion engine is not yet implemented.
#if defined(JS_CODEGEN_NONE) || defined(JS_CODEGEN_ARM64)
    return false;
#else
    return cx->runtime()->options().ion() &&
           cx->runtime()->options().baseline() &&
           cx->runtime()->jitSupportsFloatingPoint;
#endif
}

inline bool
IsIonInlinablePC(jsbytecode* pc) {
    // CALL, FUNCALL, FUNAPPLY, EVAL, NEW (Normal Callsites)
    // GETPROP, CALLPROP, and LENGTH. (Inlined Getters)
    // SETPROP, SETNAME, SETGNAME (Inlined Setters)
    return IsCallPC(pc) || IsGetPropPC(pc) || IsSetPropPC(pc);
}

inline bool
TooManyActualArguments(unsigned nargs)
{
    return nargs > JitOptions.maxStackArgs;
}

inline bool
TooManyFormalArguments(unsigned nargs)
{
    return nargs >= SNAPSHOT_MAX_NARGS || TooManyActualArguments(nargs);
}

inline size_t
NumLocalsAndArgs(JSScript* script)
{
    size_t num = 1 /* this */ + script->nfixed();
    if (JSFunction* fun = script->functionNonDelazifying())
        num += fun->nargs();
    return num;
}

bool OffThreadCompilationAvailable(JSContext* cx);

void ForbidCompilation(JSContext* cx, JSScript* script);

void PurgeCaches(JSScript* script);
size_t SizeOfIonData(JSScript* script, mozilla::MallocSizeOf mallocSizeOf);
void DestroyJitScripts(FreeOp* fop, JSScript* script);
void TraceJitScripts(JSTracer* trc, JSScript* script);

bool JitSupportsFloatingPoint();
bool JitSupportsSimd();
bool JitSupportsAtomics();

} // namespace jit
} // namespace js

#endif /* jit_Ion_h */
