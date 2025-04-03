/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

/* [SMDOC] The WASM ABIs
 *
 * Wasm-internal ABI.
 *
 * The *Wasm-internal ABI* is the ABI a wasm function assumes when it is
 * entered, and the one it assumes when it is making a call to what it believes
 * is another wasm function.
 *
 * We pass the first function arguments in registers (GPR and FPU both) and the
 * rest on the stack, generally according to platform ABI conventions (which can
 * be hairy).  On x86-32 there are no register arguments.
 *
 * We have no callee-saves registers in the wasm-internal ABI, regardless of the
 * platform ABI conventions, though see below about InstanceReg or HeapReg.
 *
 * We return the last return value in the first return register, according to
 * platform ABI conventions.  If there is more than one return value, an area is
 * allocated in the caller's frame to receive the other return values, and the
 * address of this area is passed to the callee as the last argument.  Return
 * values except the last are stored in ascending order within this area.  Also
 * see below about alignment of this area and the values in it.
 *
 * When a function is entered, there are two incoming register values in
 * addition to the function's declared parameters: InstanceReg must have the
 * correct instance pointer, and HeapReg the correct memoryBase, for the
 * function.  (On x86-32 there is no HeapReg.)  From the instance we can get to
 * the JSContext, the instance, the MemoryBase, and many other things.  The
 * instance maps one-to-one with an instance.
 *
 * HeapReg and InstanceReg are not parameters in the usual sense, nor are they
 * callee-saves registers.  Instead they constitute global register state, the
 * purpose of which is to bias the call ABI in favor of intra-instance calls,
 * the predominant case where the caller and the callee have the same
 * InstanceReg and HeapReg values.
 *
 * With this global register state, literally no work needs to take place to
 * save and restore the instance and MemoryBase values across intra-instance
 * call boundaries.
 *
 * For inter-instance calls, in contrast, there must be an instance switch at
 * the call boundary: Before the call, the callee's instance must be loaded
 * (from a closure or from the import table), and from the instance we load the
 * callee's MemoryBase, the realm, and the JSContext.  The caller's and callee's
 * instance values must be stored into the frame (to aid unwinding), the
 * callee's realm must be stored into the JSContext, and the callee's instance
 * and MemoryBase values must be moved to appropriate registers.  After the
 * call, the caller's instance must be loaded, and from it the caller's
 * MemoryBase and realm, and the JSContext.  The realm must be stored into the
 * JSContext and the caller's instance and MemoryBase values must be moved to
 * appropriate registers.
 *
 * Direct calls to functions within the same module are always intra-instance,
 * while direct calls to imported functions are always inter-instance.  Indirect
 * calls -- call_indirect in the MVP, future call_ref and call_funcref -- may or
 * may not be intra-instance.
 *
 * call_indirect, and future call_funcref, also pass a signature value in a
 * register (even on x86-32), this is a small integer or a pointer value
 * denoting the caller's expected function signature.  The callee must compare
 * it to the value or pointer that denotes its actual signature, and trap on
 * mismatch.
 *
 * This is what the stack looks like during a call, after the callee has
 * completed the prologue:
 *
 *     |                                   |
 *     +-----------------------------------+ <-+
 *     |               ...                 |   |
 *     |      Caller's private frame       |   |
 *     +-----------------------------------+   |
 *     |   Multi-value return (optional)   |   |
 *     |               ...                 |   |
 *     +-----------------------------------+   |
 *     |       Stack args (optional)       |   |
 *     |               ...                 |   |
 *     +-----------------------------------+ -+|
 *     |          Caller instance slot     |   \
 *     |          Callee instance slot     |   | \
 *     +-----------------------------------+   |  \
 *     |       Shadowstack area (Win64)    |   |  wasm::FrameWithInstances
 *     |            (32 bytes)             |   |  /
 *     +-----------------------------------+   | /  <= SP "Before call"
 *     |          Return address           |   //   <= SP "After call"
 *     |             Saved FP          ----|--+/
 *     +-----------------------------------+ -+ <=  FP (a wasm::Frame*)
 *     |  DebugFrame, Locals, spills, etc  |
 *     |   (i.e., callee's private frame)  |
 *     |             ....                  |
 *     +-----------------------------------+    <=  SP
 *
 * The FrameWithInstances is a struct with four fields: the saved FP, the return
 * address, and the two instance slots; the shadow stack area is there only on
 * Win64 and is unused by wasm but is part of the native ABI, with which the
 * wasm ABI is mostly compatible.  The slots for caller and callee instance are
 * only populated by the instance switching code in inter-instance calls so that
 * stack unwinding can keep track of the correct instance value for each frame,
 * the instance not being obtainable from anywhere else.  Nothing in the frame
 * itself indicates directly whether the instance slots are valid - for that,
 * the return address must be used to look up a CallSite structure that carries
 * that information.
 *
 * The stack area above the return address is owned by the caller, which may
 * deallocate the area on return or choose to reuse it for subsequent calls.
 * (The baseline compiler allocates and frees the stack args area and the
 * multi-value result area per call.  Ion reuses the areas and allocates them as
 * part of the overall activation frame when the procedure is entered; indeed,
 * the multi-value return area can be anywhere within the caller's private
 * frame, not necessarily directly above the stack args.)
 *
 * If the stack args area contain references, it is up to the callee's stack map
 * to name the locations where those references exist, and the caller's stack
 * map must not (redundantly) name those locations.  (The callee's ownership of
 * this area will be crucial for making tail calls work, as the types of the
 * locations can change if the callee makes a tail call.)  If pointer values are
 * spilled by anyone into the Shadowstack area they will not be traced.
 *
 * References in the multi-return area are covered by the caller's map, as these
 * slots outlive the call.
 *
 * The address "Before call", ie the part of the FrameWithInstances above the
 * Frame, must be aligned to WasmStackAlignment, and everything follows from
 * that, with padding inserted for alignment as required for stack arguments. In
 * turn WasmStackAlignment is at least as large as the largest parameter type.
 *
 * The address of the multiple-results area is currently 8-byte aligned by Ion
 * and its alignment in baseline is uncertain, see bug 1747787.  Result values
 * are stored packed within the area in fields whose size is given by
 * ResultStackSize(ValType), this breaks alignment too.  This all seems
 * underdeveloped.
 *
 * In the wasm-internal ABI, the ARM64 PseudoStackPointer (PSP) is garbage on
 * entry but must be synced with the real SP at the point the function returns.
 *
 *
 * The Wasm Builtin ABIs.
 *
 * Also see `[SMDOC] Process-wide builtin thunk set` in WasmBuiltins.cpp.
 *
 * The *Wasm-builtin ABIs* comprise the ABIs used when wasm makes calls directly
 * to the C++ runtime (but not to the JS interpreter), including instance
 * methods, helpers for operations such as 64-bit division on 32-bit systems,
 * allocation and writer barriers, conversions to/from JS values, special
 * fast-path JS imports, and trap handling.
 *
 * The callee of a builtin call will always assume the C/C++ ABI.  Therefore
 * every volatile (caller-saves) register that wasm uses must be saved across
 * the call, the stack must be aligned as for a C/C++-ABI call before the call,
 * and any ABI registers the callee expect to have specific values must be set
 * up (eg the frame pointer, if the C/C++ ABI assumes it is set).
 *
 * Most builtin calls are straightforward: the wasm caller knows that it is
 * performing a call, and so it saves live registers, moves arguments into ABI
 * locations, etc, before calling.  Abstractions in the masm make sure to pass
 * the instance pointer to an instance "method" call and to restore the
 * InstanceReg and HeapReg after the call.  In these straightforward cases,
 * calling the builtin additionally amounts to:
 *
 *  - exiting the wasm activation
 *  - adjusting parameter values to account for platform weirdness (FP arguments
 *    are handled differently in the C/C++ ABIs on ARM and x86-32 than in the
 *    Wasm ABI)
 *  - copying stack arguments into place for the C/C++ ABIs
 *  - making the call
 *  - adjusting the return values on return
 *  - re-entering the wasm activation and returning to the wasm caller
 *
 * The steps above are performed by the *builtin thunk* for the builtin and the
 * builtin itself is said to be *thunked*.  Going via the thunk is simple and,
 * except for always having to copy stack arguments on x86-32 and the extra call
 * in the thunk, close to as fast as we can make it without heroics.  Except for
 * the arithmetic helpers on 32-bit systems, most builtins are rarely used, are
 * asm.js-specific, or are expensive anyway, and the overhead of the extra call
 * doesn't matter.
 *
 * A few builtins for special purposes are *unthunked* and fall into two
 * classes: they would normally be thunked but are used in circumstances where
 * the VM is in an unusual state; or they do their work within the activation.
 *
 * In the former class, we find the debug trap handler, which must preserve all
 * live registers because it is called in contexts where live registers have not
 * been saved; argument coercion functions, which are called while a call frame
 * is being built for a JS->Wasm or Wasm->JS call; and other routines that have
 * special needs for constructing the call.  These all exit the activation, but
 * handle the exit specially.
 *
 * In the latter class, we find two functions that abandon the VM state and
 * unwind the activation, HandleThrow and HandleTrap; and some debug print
 * functions that do not affect the VM state at all.
 *
 * To summarize, when wasm calls a builtin thunk the stack will end up looking
 * like this from within the C++ code:
 *
 *      |                         |
 *      +-------------------------+
 *      |        Wasm frame       |
 *      +-------------------------+
 *      |    Thunk frame (exit)   |
 *      +-------------------------+
 *      |   Builtin frame (C++)   |
 *      +-------------------------+  <= SP
 *
 * There is an assumption in the profiler (in initFromExitFP) that an exit has
 * left precisely one frame on the stack for the thunk itself.  There may be
 * additional assumptions elsewhere, not yet found.
 *
 * Very occasionally, Wasm will call C++ without going through the builtin
 * thunks, and this can be a source of problems.  The one case I know about
 * right now is that the JS pre-barrier filtering code is called directly from
 * Wasm, see bug 1464157.
 *
 *
 * Wasm stub ABIs.
 *
 * Also see `[SMDOC] Exported wasm functions and the jit-entry stubs` in
 * WasmJS.cpp.
 *
 * The "stub ABIs" are not properly speaking ABIs themselves, but ABI
 * converters.  An "entry" stub calls in to wasm and an "exit" stub calls out
 * from wasm.  The entry stubs must convert from whatever data formats the
 * caller has to wasm formats (and in the future must provide some kind of type
 * checking for pointer types); the exit stubs convert from wasm formats to the
 * callee's expected format.
 *
 * There are different entry paths from the JS interpreter (using the C++ ABI
 * and data formats) and from jitted JS code (using the JIT ABI and data
 * formats); indeed there is a "normal" JitEntry path ("JitEntry") that will
 * perform argument and return value conversion, and the "fast" JitEntry path
 * ("DirectCallFromJit") that is only used when it is known that the JIT will
 * only pass and receive wasm-compatible data and no conversion is needed.
 *
 * Similarly, there are different exit paths to the interpreter (using the C++
 * ABI and data formats) and to JS JIT code (using the JIT ABI and data
 * formats).  Also, builtin calls described above are themselves a type of exit,
 * and builtin thunks are properly a type of exit stub.
 *
 * Data conversions are difficult because the VM is in an intermediate state
 * when they happen, we want them to be fast when possible, and some conversions
 * can re-enter both JS code and wasm code.
 */

#ifndef wasm_frame_h
#define wasm_frame_h

#include "mozilla/Assertions.h"

#include <stddef.h>
#include <stdint.h>
#include <type_traits>

#include "jit/Registers.h"  // For js::jit::ShadowStackSpace

namespace js {
namespace wasm {

class Instance;

// Bit tag set when exiting wasm code in JitActivation's exitFP.
constexpr uintptr_t ExitFPTag = 0x1;

// wasm::Frame represents the bytes pushed by the call instruction and the
// fixed prologue generated by wasm::GenerateCallablePrologue.
//
// Across all architectures it is assumed that, before the call instruction, the
// stack pointer is WasmStackAlignment-aligned. Thus after the prologue, and
// before the function has made its stack reservation, the stack alignment is
// sizeof(Frame) % WasmStackAlignment.
//
// During MacroAssembler code generation, the bytes pushed after the wasm::Frame
// are counted by masm.framePushed. Thus, the stack alignment at any point in
// time is (sizeof(wasm::Frame) + masm.framePushed) % WasmStackAlignment.

class Frame {
  // See GenerateCallableEpilogue for why this must be
  // the first field of wasm::Frame (in a downward-growing stack).
  // It's either the caller's Frame*, for wasm callers, or the JIT caller frame
  // plus a tag otherwise.
  uint8_t* callerFP_;

  // The return address pushed by the call (in the case of ARM/MIPS the return
  // address is pushed by the first instruction of the prologue).
  void* returnAddress_;

 public:
  static constexpr uint32_t callerFPOffset() {
    return offsetof(Frame, callerFP_);
  }
  static constexpr uint32_t returnAddressOffset() {
    return offsetof(Frame, returnAddress_);
  }

  uint8_t* returnAddress() const {
    return reinterpret_cast<uint8_t*>(returnAddress_);
  }

  void** addressOfReturnAddress() {
    return reinterpret_cast<void**>(&returnAddress_);
  }

  uint8_t* rawCaller() const { return callerFP_; }

  Frame* wasmCaller() const { return reinterpret_cast<Frame*>(callerFP_); }

  uint8_t* jitEntryCaller() const { return callerFP_; }

  static const Frame* fromUntaggedWasmExitFP(const void* savedFP) {
    MOZ_ASSERT(!isExitFP(savedFP));
    return reinterpret_cast<const Frame*>(savedFP);
  }

  static bool isExitFP(const void* fp) {
    return reinterpret_cast<uintptr_t>(fp) & ExitFPTag;
  }

  static uint8_t* untagExitFP(const void* fp) {
    MOZ_ASSERT(isExitFP(fp));
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(fp) &
                                      ~ExitFPTag);
  }

  static uint8_t* addExitFPTag(const Frame* fp) {
    MOZ_ASSERT(!isExitFP(fp));
    return reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(fp) |
                                      ExitFPTag);
  }
};

static_assert(!std::is_polymorphic_v<Frame>, "Frame doesn't need a vtable.");
static_assert(sizeof(Frame) == 2 * sizeof(void*),
              "Frame is a two pointer structure");

// Note that sizeof(FrameWithInstances) does not account for ShadowStackSpace.
// Use FrameWithInstances::sizeOf() if you are not incorporating
// ShadowStackSpace through other means (eg the ABIArgIter).

class FrameWithInstances : public Frame {
  // `ShadowStackSpace` bytes will be allocated here on Win64, at higher
  // addresses than Frame and at lower addresses than the instance fields.

  // The instance area MUST be two pointers exactly.
  Instance* calleeInstance_;
  Instance* callerInstance_;

 public:
  Instance* calleeInstance() { return calleeInstance_; }
  Instance* callerInstance() { return callerInstance_; }

  constexpr static uint32_t sizeOf() {
    return sizeof(wasm::FrameWithInstances) + js::jit::ShadowStackSpace;
  }

  constexpr static uint32_t sizeOfInstanceFields() {
    return sizeof(wasm::FrameWithInstances) - sizeof(wasm::Frame);
  }

  constexpr static uint32_t calleeInstanceOffset() {
    return offsetof(FrameWithInstances, calleeInstance_) +
           js::jit::ShadowStackSpace;
  }

  constexpr static uint32_t calleeInstanceOffsetWithoutFrame() {
    return calleeInstanceOffset() - sizeof(wasm::Frame);
  }

  constexpr static uint32_t callerInstanceOffset() {
    return offsetof(FrameWithInstances, callerInstance_) +
           js::jit::ShadowStackSpace;
  }

  constexpr static uint32_t callerInstanceOffsetWithoutFrame() {
    return callerInstanceOffset() - sizeof(wasm::Frame);
  }
};

static_assert(FrameWithInstances::calleeInstanceOffsetWithoutFrame() ==
                  js::jit::ShadowStackSpace,
              "Callee instance stored right above the return address.");
static_assert(FrameWithInstances::callerInstanceOffsetWithoutFrame() ==
                  js::jit::ShadowStackSpace + sizeof(void*),
              "Caller instance stored right above the callee instance.");

static_assert(FrameWithInstances::sizeOfInstanceFields() == 2 * sizeof(void*),
              "There are only two additional slots");

#if defined(JS_CODEGEN_ARM64)
static_assert(sizeof(Frame) % 16 == 0, "frame is aligned");
#endif

}  // namespace wasm
}  // namespace js

#endif  // wasm_frame_h
