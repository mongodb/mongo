/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef A64_ASSEMBLER_A64_H_
#define A64_ASSEMBLER_A64_H_

#include <iterator>

#include "jit/arm64/vixl/Assembler-vixl.h"

#include "jit/CompactBuffer.h"
#include "jit/shared/Disassembler-shared.h"
#include "wasm/WasmTypes.h"

namespace js {
namespace jit {

// VIXL imports.
typedef vixl::Register ARMRegister;
typedef vixl::FPRegister ARMFPRegister;
using vixl::ARMBuffer;
using vixl::Instruction;

using LabelDoc = DisassemblerSpew::LabelDoc;
using LiteralDoc = DisassemblerSpew::LiteralDoc;

static const uint32_t AlignmentAtPrologue = 0;
static const uint32_t AlignmentMidPrologue = 8;
static const Scale ScalePointer = TimesEight;

// The MacroAssembler uses scratch registers extensively and unexpectedly.
// For safety, scratch registers should always be acquired using
// vixl::UseScratchRegisterScope.
static constexpr Register ScratchReg{Registers::ip0};
static constexpr ARMRegister ScratchReg64 = {ScratchReg, 64};

static constexpr Register ScratchReg2{Registers::ip1};
static constexpr ARMRegister ScratchReg2_64 = {ScratchReg2, 64};

static constexpr FloatRegister ReturnDoubleReg = {FloatRegisters::d0,
                                                  FloatRegisters::Double};
static constexpr FloatRegister ScratchDoubleReg_ = {FloatRegisters::d31,
                                                    FloatRegisters::Double};
struct ScratchDoubleScope : public AutoFloatRegisterScope {
  explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg_) {}
};

static constexpr FloatRegister ReturnFloat32Reg = {FloatRegisters::s0,
                                                   FloatRegisters::Single};
static constexpr FloatRegister ScratchFloat32Reg_ = {FloatRegisters::s31,
                                                     FloatRegisters::Single};
struct ScratchFloat32Scope : public AutoFloatRegisterScope {
  explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg_) {}
};

#ifdef ENABLE_WASM_SIMD
static constexpr FloatRegister ReturnSimd128Reg = {FloatRegisters::v0,
                                                   FloatRegisters::Simd128};
static constexpr FloatRegister ScratchSimd128Reg = {FloatRegisters::v31,
                                                    FloatRegisters::Simd128};
struct ScratchSimd128Scope : public AutoFloatRegisterScope {
  explicit ScratchSimd128Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchSimd128Reg) {}
};
#else
struct ScratchSimd128Scope : public AutoFloatRegisterScope {
  explicit ScratchSimd128Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg_) {
    MOZ_CRASH("SIMD not enabled");
  }
};
#endif

static constexpr Register InvalidReg{Registers::Invalid};
static constexpr FloatRegister InvalidFloatReg = {};

static constexpr Register OsrFrameReg{Registers::x3};
static constexpr Register CallTempReg0{Registers::x9};
static constexpr Register CallTempReg1{Registers::x10};
static constexpr Register CallTempReg2{Registers::x11};
static constexpr Register CallTempReg3{Registers::x12};
static constexpr Register CallTempReg4{Registers::x13};
static constexpr Register CallTempReg5{Registers::x14};

static constexpr Register PreBarrierReg{Registers::x1};

static constexpr Register InterpreterPCReg{Registers::x9};

static constexpr Register ReturnReg{Registers::x0};
static constexpr Register64 ReturnReg64(ReturnReg);
static constexpr Register JSReturnReg{Registers::x2};
static constexpr Register FramePointer{Registers::fp};
static constexpr Register ZeroRegister{Registers::sp};
static constexpr ARMRegister ZeroRegister64 = {Registers::sp, 64};
static constexpr ARMRegister ZeroRegister32 = {Registers::sp, 32};

// [SMDOC] AArch64 Stack Pointer and Pseudo Stack Pointer conventions
//
//                               ================
//
// Stack pointer (SP), PseudoStackPointer (PSP), and RealStackPointer:
//
// The ARM64 real SP has a constraint: it must be 16-byte aligned whenever it
// is used as the base pointer for a memory access.  (SP+offset need not be
// 16-byte aligned, but the SP value itself must be.)  The SP register may
// take on unaligned values but may not be used for a memory access while it
// is unaligned.
//
// Stack-alignment checking can be enabled or disabled by a control register;
// however that register cannot be modified by user space.  We have to assume
// stack alignment checking is enabled, and that does usually appear to be the
// case.  See the ARM Architecture Reference Manual, "D1.8.2 SP alignment
// checking", for further details.
//
// A second constraint is forced upon us by the ARM64 ABI.  This requires that
// all accesses to the stack must be at or above SP.  Accesses below SP are
// strictly forbidden, presumably because the kernel might use that area of
// memory for its own purposes -- in particular, signal delivery -- and hence
// it may get trashed at any time.
//
// Note this doesn't mean that accesses to the stack must be based off
// register SP.  Only that the effective addresses must be >= SP, regardless
// of how the address is formed.
//
// In order to allow word-wise pushes and pops, some of our ARM64 jits
// (JS-Baseline, JS-Ion, and Wasm-Ion, but not Wasm-Baseline or
// Wasm-Cranelift) dedicate x28 to be used as a PseudoStackPointer (PSP).
// Initially the PSP will have the same value as the SP.  Code can, if it
// wants, push a single word by subtracting 8 from the PSP, doing SP := PSP,
// then storing the value at PSP+0.  Given other constraints on the alignment
// of the SP at function call boundaries, this works out OK, at the cost of
// the two extra instructions per push / pop.
//
// This is all a bit messy, and is probably not robustly adhered to.  However,
// the following appear to be the intended, and mostly implemented, current
// invariants:
//
// (1) PSP is "primary", SP is "secondary".  Most stack refs are
//     PSP-relative. SP-relative is rare and (obviously) only done when we
//     know that SP is aligned.
//
// (2) At all times, the relationship SP <= PSP is maintained.  The fact that
//     SP may validly be less than PSP means that pushes on the stack force
//     the two values to become equal, by copying PSP into SP.  However, pops
//     behave differently: PSP moves back up and SP stays the same, since that
//     doesn't break the SP <= PSP invariant.
//
// (3) However, immediately before a call instruction, SP and PSP must be the
//     same.  To enforce this, PSP is copied into SP by the arm64-specific
//     MacroAssembler::call routines.
//
// (4) Also, after a function has returned, it is expected that SP holds the
//     "primary" value.  How exactly this is implemented remains not entirely
//     clear and merits further investigation.  The following points are
//     believed to be relevant:
//
//     - For calls to functions observing the system AArch64 ABI, PSP (x28) is
//       callee-saved.  That, combined with (3) above, implies SP == PSP
//       immediately after the call returns.
//
//     - JIT-generated routines return using MacroAssemblerCompat::retn, and
//       that copies PSP into SP (bizarrely; this would make more sense if it
//       copied SP into PSP); but in any case, the point is that they are the
//       same at the point that the return instruction executes.
//
//     - MacroAssembler::callWithABIPost copies PSP into SP after the return
//       of a call requiring dynamic alignment.
//
//     Given the above, it is unclear exactly where in the return sequence it
//     is expected that SP == PSP, and also whether it is the callee or caller
//     that is expected to enforce it.
//
// In general it would be nice to be able to move (at some time in the future,
// not now) to a world where *every* assignment to PSP or SP is followed
// immediately by a copy into the other register.  That would make all
// required correctness proofs trivial in the sense that it would require only
// local inspection of code immediately following (dominated by) any such
// assignment.  For the moment, however, this is a guideline, not a hard
// requirement.
//
//                               ================
//
// Mechanics of keeping the stack pointers in sync:
//
// The following two methods require that the masm's SP has been set to the PSP
// with MacroAssembler::SetStackPointer64(PseudoStackPointer64), or they will be
// no-ops.  The setup is performed manually by the jits after creating the masm.
//
// * MacroAssembler::syncStackPtr() performs SP := PSP, presumably after PSP has
//   been updated, so SP needs to move too.  This is used pretty liberally
//   throughout the code base.
//
// * MacroAssembler::initPseudoStackPtr() performs PSP := SP.  This can be used
//   after calls to non-ABI compliant code; it's not used much.
//
// In the ARM64 assembler there is a function Instruction::IsStackPtrSync() that
// recognizes the instruction emitted by syncStackPtr(), and this is used to
// skip that instruction a few places, should it be present, in the JS JIT where
// code is generated to deal with toggled calls.
//
// In various places there are calls to MacroAssembler::syncStackPtr() which
// appear to be redundant.  Investigation shows that they often are redundant,
// but not always.  Finding and removing such redundancies would be quite some
// work, so we live for now with the occasional redundant update.  Perusal of
// the Cortex-A55 and -A72 optimization guides shows no evidence that such
// assignments are any more expensive than assignments between vanilla integer
// registers, so the costs of such redundant updates are assumed to be small.
//
// Invariants on the PSP at function call boundaries:
//
// It *appears* that the following invariants exist:
//
// * On entry to JIT code, PSP == SP, ie the stack pointer is transmitted via
//   both registers.
//
// * On entry to C++ code, PSP == SP.  Certainly it appears that all calls
//   created by the MacroAssembler::call(..) routines perform 'syncStackPtr'
//   immediately before the call, and all ABI calls are routed through the
//   MacroAssembler::call layer.
//
// * The stubs generated by WasmStubs.cpp assume that, on entry, SP is the
//   active stack pointer and that PSP is dead.
//
// * The PSP is non-volatile (callee-saved).  Along a normal return path from
//   JIT code, simply having PSP == SP on exit is correct, since the exit SP is
//   the same as the entry SP by the JIT ABI.
//
// * Call-outs to non-JIT C++ code do not need to set up the PSP (it won't be
//   used), and will not need to restore the PSP on return because x28 is
//   non-volatile in the ARM64 ABI.
//
//                               ================
//
// Future cleanups to the SP-vs-PSP machinery:
//
// Currently we have somewhat unclear invariants, which are not obviously
// always enforced, and which may require complex non-local reasoning.
// Auditing the code to ensure that the invariants always hold, whilst not
// generating duplicate syncs, is close to impossible.  A future rework to
// tidy this might be as follows.  (This suggestion pertains the the entire
// JIT complex: all of the JS compilers, wasm compilers, stub generators,
// regexp compilers, etc).
//
// Currently we have that, in JIT-generated code, PSP is "primary" and SP is
// "secondary", meaning that PSP has the "real" stack pointer value and SP is
// updated whenever PSP acquires a lower value, so as to ensure that SP <= PSP.
// An exception to this scheme is the stubs code generated by WasmStubs.cpp,
// which assumes that SP is "primary" and PSP is dead.
//
// It might give us an easier incremental path to eventually removing PSP
// entirely if we switched to having SP always be the primary.  That is:
//
// (1) SP is primary, PSP is secondary
// (2) After any assignment to SP, it is copied into PSP
// (3) All (non-frame-pointer-based) stack accesses are PSP-relative
//     (as at present)
//
// This would have the effect that:
//
// * It would reinstate the invariant that on all targets, the "real" SP value
//   is in the ABI-and-or-hardware-mandated stack pointer register.
//
// * It would give us a simple story about calls and returns:
//   - for calls to non-JIT generated code (viz, C++ etc), we need no extra
//     copies, because PSP (x28) is callee-saved
//   - for calls to JIT-generated code, we need no extra copies, because of (2)
//     above
//
// * We could incrementally migrate those parts of the code generator where we
//   know that SP is 16-aligned, to use SP- rather than PSP-relative accesses
//
// * The consistent use of (2) would remove the requirement to have to perform
//   path-dependent reasoning (for paths in the generated code, not in the
//   compiler) when reading/understanding the code.
//
// * x28 would become free for use by stubs and the baseline compiler without
//   having to worry about interoperating with code that expects x28 to hold a
//   valid PSP.
//
// One might ask what mechanical checks we can add to ensure correctness, rather
// than having to verify these invariants by hand indefinitely.  Maybe some
// combination of:
//
// * In debug builds, compiling-in assert(SP == PSP) at critical places.  This
//   can be done using the existing `assertStackPtrsSynced` function.
//
// * In debug builds, scanning sections of generated code to ensure no
//   SP-relative stack accesses have been created -- for some sections, at
//   least every assignment to SP is immediately followed by a copy to x28.
//   This would also facilitate detection of duplicate syncs.
//
//                               ================
//
// Other investigative notes, for the code base at present:
//
// * Some disassembly dumps suggest that we sync the stack pointer too often.
//   This could be the result of various pieces of code working at cross
//   purposes when syncing the stack pointer, or of not paying attention to the
//   precise invariants.
//
// * As documented in RegExpNativeMacroAssembler.cpp, function
//   SMRegExpMacroAssembler::createStackFrame:
//
//   // ARM64 communicates stack address via SP, but uses a pseudo-sp (PSP) for
//   // addressing.  The register we use for PSP may however also be used by
//   // calling code, and it is nonvolatile, so save it.  Do this as a special
//   // case first because the generic save/restore code needs the PSP to be
//   // initialized already.
//
//   and also in function SMRegExpMacroAssembler::exitHandler:
//
//   // Restore the saved value of the PSP register, this value is whatever the
//   // caller had saved in it, not any actual SP value, and it must not be
//   // overwritten subsequently.
//
//   The original source for these comments was a patch for bug 1445907.
//
// * MacroAssembler-arm64.h has an interesting comment in the retn()
//   function:
//
//   syncStackPtr();  // SP is always used to transmit the stack between calls.
//
//   Same comment at abiret() in that file, and in MacroAssembler-arm64.cpp,
//   at callWithABIPre and callWithABIPost.
//
// * In Trampoline-arm64.cpp function JitRuntime::generateVMWrapper we find
//
//   // SP is used to transfer stack across call boundaries.
//   masm.initPseudoStackPtr();
//
//   after the return point of a callWithVMWrapper.  The only reasonable
//   conclusion from all those (assuming they are right) is that SP == PSP.
//
// * Wasm-Baseline does not use the PSP, but as Wasm-Ion code requires SP==PSP
//   and tiered code can have Baseline->Ion calls, Baseline will set PSP=SP
//   before a call to wasm code.  When the optimized tier is created by
//   Cranelift this is not necessary.
//
//                               ================

// StackPointer is intentionally undefined on ARM64 to prevent misuse: using
// sp as a base register is only valid if sp % 16 == 0.
static constexpr Register RealStackPointer{Registers::sp};

static constexpr Register PseudoStackPointer{Registers::x28};
static constexpr ARMRegister PseudoStackPointer64 = {Registers::x28, 64};
static constexpr ARMRegister PseudoStackPointer32 = {Registers::x28, 32};

static constexpr Register IntArgReg0{Registers::x0};
static constexpr Register IntArgReg1{Registers::x1};
static constexpr Register IntArgReg2{Registers::x2};
static constexpr Register IntArgReg3{Registers::x3};
static constexpr Register IntArgReg4{Registers::x4};
static constexpr Register IntArgReg5{Registers::x5};
static constexpr Register IntArgReg6{Registers::x6};
static constexpr Register IntArgReg7{Registers::x7};
static constexpr Register HeapReg{Registers::x21};

// Define unsized Registers.
#define DEFINE_UNSIZED_REGISTERS(N) \
  static constexpr Register r##N{Registers::x##N};
REGISTER_CODE_LIST(DEFINE_UNSIZED_REGISTERS)
#undef DEFINE_UNSIZED_REGISTERS
static constexpr Register ip0{Registers::x16};
static constexpr Register ip1{Registers::x17};
static constexpr Register fp{Registers::x29};
static constexpr Register lr{Registers::x30};
static constexpr Register rzr{Registers::xzr};

// Import VIXL registers into the js::jit namespace.
#define IMPORT_VIXL_REGISTERS(N)                  \
  static constexpr ARMRegister w##N = vixl::w##N; \
  static constexpr ARMRegister x##N = vixl::x##N;
REGISTER_CODE_LIST(IMPORT_VIXL_REGISTERS)
#undef IMPORT_VIXL_REGISTERS
static constexpr ARMRegister wzr = vixl::wzr;
static constexpr ARMRegister xzr = vixl::xzr;
static constexpr ARMRegister wsp = vixl::wsp;
static constexpr ARMRegister sp = vixl::sp;

// Import VIXL VRegisters into the js::jit namespace.
#define IMPORT_VIXL_VREGISTERS(N)                   \
  static constexpr ARMFPRegister s##N = vixl::s##N; \
  static constexpr ARMFPRegister d##N = vixl::d##N;
REGISTER_CODE_LIST(IMPORT_VIXL_VREGISTERS)
#undef IMPORT_VIXL_VREGISTERS

static constexpr ValueOperand JSReturnOperand = ValueOperand(JSReturnReg);

// Registerd used in RegExpMatcher instruction (do not use JSReturnOperand).
static constexpr Register RegExpMatcherRegExpReg = CallTempReg0;
static constexpr Register RegExpMatcherStringReg = CallTempReg1;
static constexpr Register RegExpMatcherLastIndexReg = CallTempReg2;

// Registerd used in RegExpTester instruction (do not use ReturnReg).
static constexpr Register RegExpTesterRegExpReg = CallTempReg0;
static constexpr Register RegExpTesterStringReg = CallTempReg1;
static constexpr Register RegExpTesterLastIndexReg = CallTempReg2;

static constexpr Register JSReturnReg_Type = r3;
static constexpr Register JSReturnReg_Data = r2;

static constexpr FloatRegister NANReg = {FloatRegisters::d14,
                                         FloatRegisters::Single};
// N.B. r8 isn't listed as an aapcs temp register, but we can use it as such
// because we never use return-structs.
static constexpr Register CallTempNonArgRegs[] = {r8,  r9,  r10, r11,
                                                  r12, r13, r14, r15};
static const uint32_t NumCallTempNonArgRegs = std::size(CallTempNonArgRegs);

static constexpr uint32_t JitStackAlignment = 16;

static constexpr uint32_t JitStackValueAlignment =
    JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 &&
                  JitStackValueAlignment >= 1,
              "Stack alignment should be a non-zero multiple of sizeof(Value)");

static constexpr uint32_t SimdMemoryAlignment = 16;

static_assert(CodeAlignment % SimdMemoryAlignment == 0,
              "Code alignment should be larger than any of the alignments "
              "which are used for "
              "the constant sections of the code buffer.  Thus it should be "
              "larger than the "
              "alignment for SIMD constants.");

static const uint32_t WasmStackAlignment = SimdMemoryAlignment;
static const uint32_t WasmTrapInstructionLength = 4;

// See comments in wasm::GenerateFunctionPrologue.  The difference between these
// is the size of the largest callable prologue on the platform.
static constexpr uint32_t WasmCheckedCallEntryOffset = 0u;
static constexpr uint32_t WasmCheckedTailEntryOffset = 16u;

class Assembler : public vixl::Assembler {
 public:
  Assembler() : vixl::Assembler() {}

  typedef vixl::Condition Condition;

  void finish();
  bool appendRawCode(const uint8_t* code, size_t numBytes);
  bool reserve(size_t size);
  bool swapBuffer(wasm::Bytes& bytes);

  // Emit the jump table, returning the BufferOffset to the first entry in the
  // table.
  BufferOffset emitExtendedJumpTable();
  BufferOffset ExtendedJumpTable_;
  void executableCopy(uint8_t* buffer);

  BufferOffset immPool(ARMRegister dest, uint8_t* value, vixl::LoadLiteralOp op,
                       const LiteralDoc& doc,
                       ARMBuffer::PoolEntry* pe = nullptr);
  BufferOffset immPool64(ARMRegister dest, uint64_t value,
                         ARMBuffer::PoolEntry* pe = nullptr);
  BufferOffset fImmPool(ARMFPRegister dest, uint8_t* value,
                        vixl::LoadLiteralOp op, const LiteralDoc& doc);
  BufferOffset fImmPool64(ARMFPRegister dest, double value);
  BufferOffset fImmPool32(ARMFPRegister dest, float value);

  uint32_t currentOffset() const { return nextOffset().getOffset(); }

  void bind(Label* label) { bind(label, nextOffset()); }
  void bind(Label* label, BufferOffset boff);
  void bind(CodeLabel* label) { label->target()->bind(currentOffset()); }

  void setUnlimitedBuffer() { armbuffer_.setUnlimited(); }
  bool oom() const {
    return AssemblerShared::oom() || armbuffer_.oom() ||
           jumpRelocations_.oom() || dataRelocations_.oom();
  }

  void copyJumpRelocationTable(uint8_t* dest) const {
    if (jumpRelocations_.length()) {
      memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
    }
  }
  void copyDataRelocationTable(uint8_t* dest) const {
    if (dataRelocations_.length()) {
      memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
    }
  }

  size_t jumpRelocationTableBytes() const { return jumpRelocations_.length(); }
  size_t dataRelocationTableBytes() const { return dataRelocations_.length(); }
  size_t bytesNeeded() const {
    return SizeOfCodeGenerated() + jumpRelocationTableBytes() +
           dataRelocationTableBytes();
  }

  void processCodeLabels(uint8_t* rawCode) {
    for (const CodeLabel& label : codeLabels_) {
      Bind(rawCode, label);
    }
  }

  static void UpdateLoad64Value(Instruction* inst0, uint64_t value);

  static void Bind(uint8_t* rawCode, const CodeLabel& label) {
    auto mode = label.linkMode();
    size_t patchAtOffset = label.patchAt().offset();
    size_t targetOffset = label.target().offset();

    if (mode == CodeLabel::MoveImmediate) {
      Instruction* inst = (Instruction*)(rawCode + patchAtOffset);
      Assembler::UpdateLoad64Value(inst, (uint64_t)(rawCode + targetOffset));
    } else {
      *reinterpret_cast<const void**>(rawCode + patchAtOffset) =
          rawCode + targetOffset;
    }
  }

  void retarget(Label* cur, Label* next);

  // The buffer is about to be linked. Ensure any constant pools or
  // excess bookkeeping has been flushed to the instruction stream.
  void flush() { armbuffer_.flushPool(); }

  void comment(const char* msg) {
#ifdef JS_DISASM_ARM64
    spew_.spew("; %s", msg);
#endif
  }

  void setPrinter(Sprinter* sp) {
#ifdef JS_DISASM_ARM64
    spew_.setPrinter(sp);
#endif
  }

  static bool SupportsFloatingPoint() { return true; }
  static bool SupportsUnalignedAccesses() { return true; }
  static bool SupportsFastUnalignedAccesses() { return true; }
  static bool SupportsWasmSimd() { return true; }

  static bool HasRoundInstruction(RoundingMode mode) {
    switch (mode) {
      case RoundingMode::Up:
      case RoundingMode::Down:
      case RoundingMode::NearestTiesToEven:
      case RoundingMode::TowardsZero:
        return true;
    }
    MOZ_CRASH("unexpected mode");
  }

 protected:
  // Add a jump whose target is unknown until finalization.
  // The jump may not be patched at runtime.
  void addPendingJump(BufferOffset src, ImmPtr target, RelocationKind kind);

 public:
  static uint32_t PatchWrite_NearCallSize() { return 4; }

  static uint32_t NopSize() { return 4; }

  static void PatchWrite_NearCall(CodeLocationLabel start,
                                  CodeLocationLabel toCall);
  static void PatchDataWithValueCheck(CodeLocationLabel label,
                                      PatchedImmPtr newValue,
                                      PatchedImmPtr expected);

  static void PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                      ImmPtr expected);

  static void PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm) {
    // Raw is going to be the return address.
    uint32_t* raw = (uint32_t*)label.raw();
    // Overwrite the 4 bytes before the return address, which will end up being
    // the call instruction.
    *(raw - 1) = imm.value;
  }
  static uint32_t AlignDoubleArg(uint32_t offset) {
    MOZ_CRASH("AlignDoubleArg()");
  }
  static uintptr_t GetPointer(uint8_t* ptr) {
    Instruction* i = reinterpret_cast<Instruction*>(ptr);
    uint64_t ret = i->Literal64();
    return ret;
  }

  // Toggle a jmp or cmp emitted by toggledJump().
  static void ToggleToJmp(CodeLocationLabel inst_);
  static void ToggleToCmp(CodeLocationLabel inst_);
  static void ToggleCall(CodeLocationLabel inst_, bool enabled);

  static void TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);
  static void TraceDataRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);

  void assertNoGCThings() const {
#ifdef DEBUG
    MOZ_ASSERT(dataRelocations_.length() == 0);
    for (auto& j : pendingJumps_) {
      MOZ_ASSERT(j.kind == RelocationKind::HARDCODED);
    }
#endif
  }

 public:
  // A Jump table entry is 2 instructions, with 8 bytes of raw data
  static const size_t SizeOfJumpTableEntry = 16;

  struct JumpTableEntry {
    uint32_t ldr;
    uint32_t br;
    void* data;

    Instruction* getLdr() { return reinterpret_cast<Instruction*>(&ldr); }
  };

  // Offset of the patchable target for the given entry.
  static const size_t OffsetOfJumpTableEntryPointer = 8;

 public:
  void writeCodePointer(CodeLabel* label) {
    armbuffer_.assertNoPoolAndNoNops();
    uintptr_t x = uintptr_t(-1);
    BufferOffset off = EmitData(&x, sizeof(uintptr_t));
    label->patchAt()->bind(off.getOffset());
  }

  void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                   const Disassembler::HeapAccess& heapAccess) {
    MOZ_CRASH("verifyHeapAccessDisassembly");
  }

 protected:
  // Structure for fixing up pc-relative loads/jumps when the machine
  // code gets moved (executable copy, gc, etc.).
  struct RelativePatch {
    BufferOffset offset;
    void* target;
    RelocationKind kind;

    RelativePatch(BufferOffset offset, void* target, RelocationKind kind)
        : offset(offset), target(target), kind(kind) {}
  };

  // List of jumps for which the target is either unknown until finalization,
  // or cannot be known due to GC. Each entry here requires a unique entry
  // in the extended jump table, and is patched at finalization.
  js::Vector<RelativePatch, 8, SystemAllocPolicy> pendingJumps_;

  // Final output formatters.
  CompactBufferWriter jumpRelocations_;
  CompactBufferWriter dataRelocations_;
};

static const uint32_t NumIntArgRegs = 8;
static const uint32_t NumFloatArgRegs = 8;

class ABIArgGenerator {
 public:
  ABIArgGenerator()
      : intRegIndex_(0), floatRegIndex_(0), stackOffset_(0), current_() {}

  ABIArg next(MIRType argType);
  ABIArg& current() { return current_; }
  uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }
  void increaseStackOffset(uint32_t bytes) { stackOffset_ += bytes; }

 protected:
  unsigned intRegIndex_;
  unsigned floatRegIndex_;
  uint32_t stackOffset_;
  ABIArg current_;
};

// These registers may be volatile or nonvolatile.
static constexpr Register ABINonArgReg0 = r8;
static constexpr Register ABINonArgReg1 = r9;
static constexpr Register ABINonArgReg2 = r10;
static constexpr Register ABINonArgReg3 = r11;

// This register may be volatile or nonvolatile. Avoid d31 which is the
// ScratchDoubleReg_.
static constexpr FloatRegister ABINonArgDoubleReg = {FloatRegisters::s16,
                                                     FloatRegisters::Single};

// These registers may be volatile or nonvolatile.
// Note: these three registers are all guaranteed to be different
static constexpr Register ABINonArgReturnReg0 = r8;
static constexpr Register ABINonArgReturnReg1 = r9;
static constexpr Register ABINonVolatileReg{Registers::x19};

// This register is guaranteed to be clobberable during the prologue and
// epilogue of an ABI call which must preserve both ABI argument, return
// and non-volatile registers.
static constexpr Register ABINonArgReturnVolatileReg = lr;

// TLS pointer argument register for WebAssembly functions. This must not alias
// any other register used for passing function arguments or return values.
// Preserved by WebAssembly functions.  Must be nonvolatile.
static constexpr Register WasmTlsReg{Registers::x23};

// Registers used for wasm table calls. These registers must be disjoint
// from the ABI argument registers, WasmTlsReg and each other.
static constexpr Register WasmTableCallScratchReg0 = ABINonArgReg0;
static constexpr Register WasmTableCallScratchReg1 = ABINonArgReg1;
static constexpr Register WasmTableCallSigReg = ABINonArgReg2;
static constexpr Register WasmTableCallIndexReg = ABINonArgReg3;

// Register used as a scratch along the return path in the fast js -> wasm stub
// code.  This must not overlap ReturnReg, JSReturnOperand, or WasmTlsReg.  It
// must be a volatile register.
static constexpr Register WasmJitEntryReturnScratch = r9;

// Register used to store a reference to an exception thrown by Wasm to an
// exception handling block. Should not overlap with WasmTlsReg.
static constexpr Register WasmExceptionReg = ABINonArgReg2;

static inline bool GetIntArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs,
                                Register* out) {
  if (usedIntArgs >= NumIntArgRegs) {
    return false;
  }
  *out = Register::FromCode(usedIntArgs);
  return true;
}

static inline bool GetFloatArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs,
                                  FloatRegister* out) {
  if (usedFloatArgs >= NumFloatArgRegs) {
    return false;
  }
  *out = FloatRegister::FromCode(usedFloatArgs);
  return true;
}

// Get a register in which we plan to put a quantity that will be used as an
// integer argument.  This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool GetTempRegForIntArg(uint32_t usedIntArgs,
                                       uint32_t usedFloatArgs, Register* out) {
  if (GetIntArgReg(usedIntArgs, usedFloatArgs, out)) {
    return true;
  }
  // Unfortunately, we have to assume things about the point at which
  // GetIntArgReg returns false, because we need to know how many registers it
  // can allocate.
  usedIntArgs -= NumIntArgRegs;
  if (usedIntArgs >= NumCallTempNonArgRegs) {
    return false;
  }
  *out = CallTempNonArgRegs[usedIntArgs];
  return true;
}

inline Imm32 Imm64::firstHalf() const { return low(); }

inline Imm32 Imm64::secondHalf() const { return hi(); }

// Forbids nop filling for testing purposes. Not nestable.
class AutoForbidNops {
 protected:
  Assembler* asm_;

 public:
  explicit AutoForbidNops(Assembler* asm_) : asm_(asm_) { asm_->enterNoNops(); }
  ~AutoForbidNops() { asm_->leaveNoNops(); }
};

// Forbids pool generation during a specified interval. Not nestable.
class AutoForbidPoolsAndNops : public AutoForbidNops {
 public:
  AutoForbidPoolsAndNops(Assembler* asm_, size_t maxInst)
      : AutoForbidNops(asm_) {
    asm_->enterNoPool(maxInst);
  }
  ~AutoForbidPoolsAndNops() { asm_->leaveNoPool(); }
};

}  // namespace jit
}  // namespace js

#endif  // A64_ASSEMBLER_A64_H_
