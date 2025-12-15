/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_PortableBaselineInterpret_h
#define vm_PortableBaselineInterpret_h

/*
 * [SMDOC] Portable Baseline Interpreter
 * =====================================
 *
 * The Portable Baseline Interpreter (PBL) is a portable interpreter
 * that supports executing ICs by directly interpreting CacheIR.
 *
 * This interpreter tier fits into the hierarchy between the C++
 * interpreter, which is fully generic and does not specialize with
 * ICs, and the native baseline interpreter, which does attach and
 * execute ICs but requires native codegen (JIT). The distinguishing
 * feature of PBL is that it *does not require codegen*: it can run on
 * any platform for which SpiderMonkey supports an interpreter-only
 * build. This is useful both for platforms that do not support
 * runtime addition of new code (e.g., running within a WebAssembly
 * module with a `wasm32-wasi` build) or may disallow it for security
 * reasons.
 *
 * The main idea of PBL is to emulate, as much as possible, how the
 * native baseline interpreter works, so that the rest of the engine
 * can work the same either way. The main aspect of this "emulation"
 * comes with stack frames: unlike the native blinterp and JIT tiers,
 * we cannot use the machine stack, because we are still executing in
 * portable C++ code and the platform's C++ compiler controls the
 * machine stack's layout. Instead, we use an auxiliary stack.
 *
 * Auxiliary Stack
 * ---------------
 *
 * PBL creates baseline stack frames (see `BaselineFrame` and related
 * structs) on an *auxiliary stack*, contiguous memory allocated and
 * owned by the JSRuntime.
 *
 * This stack operates nearly identically to the machine stack: it
 * grows downward, we push stack frames, we maintain a linked list of
 * frame pointers, and a series of contiguous frames form a
 * `JitActivation`, with the most recent activation reachable from the
 * `JSContext`. The only actual difference is that the return address
 * slots in the frame layouts are always null pointers, because there
 * is no need to save return addresses: we always know where we are
 * going to return to (either post-IC code -- the return point of
 * which is known because we actually do a C++-level call from the
 * JSOp interpreter to the IC interpreter -- or to dispatch the next
 * JSOp).
 *
 * The same invariants as for native baseline code apply here: when we
 * are in `PortableBaselineInterpret` (the PBL interpreter body) or
 * `ICInterpretOps` (the IC interpreter) or related helpers, it is as
 * if we are in JIT code, and local state determines the top-of-stack
 * and innermost frame. The activation is not "finished" and cannot be
 * traversed. When we need to call into the rest of SpiderMonkey, we
 * emulate how that would work in JIT code, via an exit frame (that
 * would ordinarily be pushed by a trampoline) and saving that frame
 * as the exit-frame pointer in the VM state.
 *
 * To add a little compile-time enforcement of this strategy, and
 * ensure that we don't accidentally call something that will want to
 * traverse the (in-progress and not-completed) JIT activation, we use
 * a helper class `VMFrame` that pushes and pops the exit frame,
 * wrapping the callsite into the rest of SM with an RAII idiom. Then,
 * we *hide the `JSContext`*, and rely on the idiom that `cx` is
 * passed to anything that can GC or otherwise observe the JIT
 * state. The `JSContext` is passed in as `cx_`, and we name the
 * `VMFrame` local `cx` in the macro that invokes it; this `cx` then
 * has an implicit conversion to a `JSContext*` value and reveals the
 * real context.
 *
 * Interpreter Loops
 * -----------------
 *
 * There are two interpreter loops: the JSOp interpreter and the
 * CacheIR interpreter. These closely correspond to (i) the blinterp
 * body that is generated at startup for the native baseline
 * interpreter, and (ii) an interpreter version of the code generated
 * by the `BaselineCacheIRCompiler`, respectively.
 *
 * Execution begins in the JSOp interpreter, and for any op(*) that
 * has an IC site (`JOF_IC` flag), we invoke the IC interpreter. The
 * IC interpreter runs a loop that traverses the IC stub chain, either
 * reaching CacheIR bytecode and executing it in a virtual machine, or
 * reaching the fallback stub and executing that (likely pushing an
 * exit frame and calling into the rest of SpiderMonkey).
 *
 * (*) As an optimization, some opcodes that would have IC sites in
 * native baseline skip their IC chains and run generic code instead
 * in PBL. See "Hybrid IC mode" below for more details.
 *
 * IC Interpreter State
 * --------------------
 *
 * While the JS opcode interpreter's abstract machine model and its
 * mapping of those abstract semantics to real machine state are
 * well-defined (by the other baseline tiers), the IC interpreter's
 * mapping is less so. When executing in native baseline tiers,
 * CacheIR is compiled to machine code that undergoes register
 * allocation and several optimizations (e.g., handling constants
 * specially, and eliding type-checks on values when we know their
 * actual types). No other interpreter for CacheIR exists, so we get
 * to define how we map the semantics to interpreter state.
 *
 * We choose to keep an array of uint64_t values as "virtual
 * registers", each corresponding to a particular OperandId, and we
 * store the same values that would exist in the native machine
 * registers. In other words, we do not do any sort of register
 * allocation or reclamation of storage slots, because we don't have
 * any lookahead in the interpreter. We rely on the typesafe writer
 * API, with newtype'd wrappers for different kinds of values
 * (`ValOperandId`, `ObjOperandId`, `Int32OperandId`, etc.), producing
 * typesafe CacheIR bytecode, in order to properly store and interpret
 * unboxed values in the virtual registers.
 *
 * There are several subtle details usually handled by register
 * allocation in the CacheIR compilers that need to be handled here
 * too, mainly around input arguments and restoring state when
 * chaining to the next IC stub. IC callsites place inputs into the
 * first N OperandId registers directly, corresponding to what the
 * CacheIR expects. There are some CacheIR opcodes that mutate their
 * argument in-place (e.g., guarding that a Value is an Object strips
 * the tag-bits from the Value and turns it into a raw pointer), so we
 * cannot rely on these remaining unmodified if we need to invoke the
 * next IC in the chain; instead, we save and restore the first N
 * values in the chain-walking loop (according to the arity of the IC
 * kind).
 *
 * Optimizations
 * ------------
 *
 * There are several implementation details that are critical for
 * performance, and thus should be carefully maintained or verified
 * with any changes:
 *
 * - Caching values in locals: in order to be competitive with "native
 *   baseline interpreter", which has the advantage of using machine
 *   registers for commonly-accessed values such as the
 *   top-of-operand-stack and the JS opcode PC, we are careful to
 *   ensure that the C++ compiler can keep these values in registers
 *   in PBL as well. One might naively store `pc`, `sp`, `fp`, and the
 *   like in a context struct (of "virtual CPU registers") that is
 *   passed to e.g. the IC interpreter. This would be a mistake: if
 *   the values exist in memory, the compiler cannot "lift" them to
 *   locals that can live in registers, and so every push and pop (for
 *   example) performs a store. This overhead is significant,
 *   especially when executing more "lightweight" opcodes.
 *
 *   We make use of an important property -- the balanced-stack
 *   invariant -- so that we can pass SP *into* calls but not take an
 *   updated SP *from* them. When invoking an IC, we expect that when
 *   it returns, SP will be at the same location (one could think of
 *   SP as a "callee-saved register", though it's not usually
 *   described that way). Thus, we can avoid a dependency on a value
 *   that would have to be passed back through memory.
 *
 * - Hybrid IC mode: the fact that we *interpret* ICs now means that
 *   they are more expensive to invoke. Whereas a small IC that guards
 *   two int32 arguments, performs an int32 add, and returns might
 *   have been a handful of instructions before, and the call/ret pair
 *   would have been very fast (and easy to predict) instructions at
 *   the machine level, the setup and context transition and the
 *   CacheIR opcode dispatch overhead would likely be much slower than
 *   a generic "if both int32, add" fastpath in the interpreter case
 *   for `JSOp::Add`.
 *
 *   We thus take a hybrid approach, and include these static
 *   fastpaths for what would have been ICs in "native
 *   baseline". These are enabled by the `kHybridICs` global and may
 *   be removed in the future (transitioning back to ICs) if/when we
 *   can reduce the cost of interpreted ICs further.
 *
 *   Right now, calls and property accesses use ICs:
 *
 *   - Calls can often be special-cased with CacheIR when intrinsics
 *     are invoked. For example, a call to `String.length` can turn
 *     into a CacheIR opcode that directly reads a `JSString`'s length
 *     field.
 *   - Property accesses are so frequent, and the shape-lookup path
 *     is slow enough, that it still makes sense to guard on shape
 *     and quickly return a particular slot.
 *
 * - Static branch prediction for opcode dispatch: we adopt an
 *   interpreter optimization we call "static branch prediction": when
 *   one opcode is often followed by another, it is often more
 *   efficient to check for those specific cases first and branch
 *   directly to the case for the following opcode, doing the full
 *   switch otherwise. This is especially true when the indirect
 *   branches used by `switch` statements or computed gotos are
 *   expensive on a given platform, such as Wasm.
 *
 * - Inlining: on some platforms, calls are expensive, and we want to
 *   avoid them whenever possible. We have found that it is quite
 *   important for performance to inline the IC interpreter into the
 *   JSOp interpreter at IC sites: both functions are quite large,
 *   with significant local state, and so otherwise, each IC call
 *   involves a lot of "context switching" as the code generated by
 *   the C++ compiler saves registers and constructs a new native
 *   frame. This is certainly a code-size tradeoff, but we have
 *   optimized for speed here.
 *
 * - Amortized stack checks: a naive interpreter implementation would
 *   check for auxiliary stack overflow on every push. We instead do
 *   this once when we enter a new JS function frame, using the
 *   script's precomputed "maximum stack depth" value. We keep a small
 *   stack margin always available, so that we have enough space to
 *   push an exit frame and invoke the "over-recursed" helper (which
 *   throws an exception) when we would otherwise overflow. The stack
 *   checks take this margin into account, failing if there would be
 *   less than the margin available at any point in the called
 *   function.
 *
 * - Fastpaths for calls and returns: we are able to push and pop JS
 *   stack frames while remaining in one native (C++ interpreter
 *   function) frame, just as the C++ interpreter does. This means
 *   that there is a one-to-many mapping from native stack frame to JS
 *   stack frame. This does create some complications at points that
 *   pop frames: we might remain in the same C++ frame, or we might
 *   return at the C++ level. We handle this in a unified way for
 *   returns and exception unwinding as described below.
 *
 * Unwinding
 * ---------
 *
 * Because one C++ interpreter frame can correspond to multiple JS
 * frames, we need to disambiguate the two cases whenever leaving a
 * frame: we may need to return, or we may stay in the current
 * function and dispatch the next opcode at the caller's next PC.
 *
 * Exception unwinding compilcates this further. PBL uses the same
 * exception-handling code that native baseline does, and this code
 * computes a `ResumeFromException` struct that tells us what our new
 * stack pointer and frame pointer must be. These values could be
 * arbitrarily far "up" the stack in the current activation. It thus
 * wouldn't be sufficient to count how many JS frames we have, and
 * return at the C++ level when this reaches zero: we need to "unwind"
 * the C++ frames until we reach the appropriate JS frame.
 *
 * To solve both issues, we remember the "entry frame" when we enter a
 * new invocation of `PortableBaselineInterpret()`, and when returning
 * or unwinding, if the new frame is *above* this entry frame, we
 * return. We have an enum `PBIResult` that can encode, when
 * unwinding, *which* kind of unwinding we are doing, because when we
 * do eventually reach the C++ frame that owns the newly active JS
 * frame, we may resume into a different action depending on this
 * information.
 *
 * Completeness
 * ------------
 *
 * Whenever a new JSOp is added, the opcode needs to be added to
 * PBL. The compiler should enforce this: if no case is implemented
 * for an opcode, then the label in the computed-goto table will be
 * missing and PBL will not compile.
 *
 * In contrast, CacheIR opcodes need not be implemented right away,
 * and in fact right now most of the less-common ones are not
 * implemented by PBL. If the IC interpreter hits an unimplemented
 * opcode, it acts as if a guard had failed, and transfers to the next
 * stub in the chain. Every chain ends with a fallback stub that can
 * handle every case (it does not execute CacheIR at all, but instead
 * calls into the runtime), so this will always give the correct
 * result, albeit more slowly. Implementing the remainder of the
 * CacheIR opcodes, and new ones as they are added, is thus purely a
 * performance concern.
 *
 * PBL currently does not implement async resume into a suspended
 * generator. There is no particular reason that this cannot be
 * implemented; it just has not been done yet. Such an action will
 * currently call back into the C++ interpreter to run the resumed
 * generator body. Execution up to the first yield-point can still
 * occur in PBL, and PBL can successfully save the suspended state.
 */

#include "jspubtd.h"

#include "jit/BaselineFrame.h"
#include "jit/BaselineIC.h"
#include "jit/JitContext.h"
#include "jit/JitRuntime.h"
#include "jit/JitScript.h"
#include "vm/Interpreter.h"
#include "vm/Stack.h"

namespace js {
namespace pbl {

// Trampoline invoked by EnterJit that sets up PBL state and invokes
// the main interpreter loop.
bool PortableBaselineTrampoline(JSContext* cx, size_t argc, Value* argv,
                                size_t numActuals, size_t numFormals,
                                jit::CalleeToken calleeToken,
                                JSObject* envChain, Value* result);

// Predicate: are all conditions satisfied to allow execution within
// PBL? This depends only on properties of the function to be invoked,
// and not on other runtime state, like the current stack depth, so if
// it returns `true` once, it can be assumed to always return `true`
// for that function. See `PortableBaselineInterpreterStackCheck`
// below for a complimentary check that does not have this property.
jit::MethodStatus CanEnterPortableBaselineInterpreter(JSContext* cx,
                                                      RunState& state);

// A check for availbale stack space on the PBL auxiliary stack that
// is invoked before the main trampoline. This is required for entry
// into PBL and should be checked before invoking the trampoline
// above. Unlike `CanEnterPortableBaselineInterpreter`, the result of
// this check cannot be cached: it must be checked on each potential
// entry.
bool PortablebaselineInterpreterStackCheck(JSContext* cx, RunState& state,
                                           size_t numActualArgs);

struct State;
struct Stack;
struct StackVal;
struct StackValNative;
struct ICRegs;
class VMFrameManager;

enum class PBIResult {
  Ok,
  Error,
  Unwind,
  UnwindError,
  UnwindRet,
};

template <bool IsRestart, bool HybridICs>
PBIResult PortableBaselineInterpret(
    JSContext* cx_, State& state, Stack& stack, StackVal* sp,
    JSObject* envChain, Value* ret, jsbytecode* pc, ImmutableScriptData* isd,
    jsbytecode* restartEntryPC, jit::BaselineFrame* restartFrame,
    StackVal* restartEntryFrame, PBIResult restartCode);

uint8_t* GetPortableFallbackStub(jit::BaselineICFallbackKind kind);
uint8_t* GetICInterpreter();

} /* namespace pbl */
} /* namespace js */

#endif /* vm_PortableBaselineInterpret_h */
