/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef A64_ASSEMBLER_A64_H_
#define A64_ASSEMBLER_A64_H_

#include "jit/arm64/vixl/Assembler-vixl.h"

#include "jit/JitCompartment.h"

namespace js {
namespace jit {

// VIXL imports.
typedef vixl::Register ARMRegister;
typedef vixl::FPRegister ARMFPRegister;
using vixl::ARMBuffer;
using vixl::Instruction;

static const uint32_t AlignmentAtPrologue = 0;
static const uint32_t AlignmentMidPrologue = 8;
static const Scale ScalePointer = TimesEight;
static const uint32_t AlignmentAtAsmJSPrologue = sizeof(void*);

// The MacroAssembler uses scratch registers extensively and unexpectedly.
// For safety, scratch registers should always be acquired using
// vixl::UseScratchRegisterScope.
static constexpr Register ScratchReg = { Registers::ip0 };
static constexpr ARMRegister ScratchReg64 = { ScratchReg, 64 };

static constexpr Register ScratchReg2 = { Registers::ip1 };
static constexpr ARMRegister ScratchReg2_64 = { ScratchReg2, 64 };

static constexpr FloatRegister ScratchDoubleReg = { FloatRegisters::d31, FloatRegisters::Double };
static constexpr FloatRegister ReturnDoubleReg = { FloatRegisters::d0, FloatRegisters::Double };

static constexpr FloatRegister ReturnFloat32Reg = { FloatRegisters::s0, FloatRegisters::Single };
static constexpr FloatRegister ScratchFloat32Reg = { FloatRegisters::s31, FloatRegisters::Single };

static constexpr Register InvalidReg = { Registers::invalid_reg };
static constexpr FloatRegister InvalidFloatReg = { FloatRegisters::invalid_fpreg, FloatRegisters::Single };

static constexpr Register OsrFrameReg = { Registers::x3 };
static constexpr Register ArgumentsRectifierReg = { Registers::x8 };
static constexpr Register CallTempReg0 = { Registers::x9 };
static constexpr Register CallTempReg1 = { Registers::x10 };
static constexpr Register CallTempReg2 = { Registers::x11 };
static constexpr Register CallTempReg3 = { Registers::x12 };
static constexpr Register CallTempReg4 = { Registers::x13 };
static constexpr Register CallTempReg5 = { Registers::x14 };

static constexpr Register PreBarrierReg = { Registers::x1 };

static constexpr Register ReturnReg = { Registers::x0 };
static constexpr Register JSReturnReg = { Registers::x2 };
static constexpr Register FramePointer = { Registers::fp };
static constexpr Register ZeroRegister = { Registers::sp };
static constexpr ARMRegister ZeroRegister64 = { Registers::sp, 64 };
static constexpr ARMRegister ZeroRegister32 = { Registers::sp, 32 };

static constexpr FloatRegister ReturnSimd128Reg = InvalidFloatReg;
static constexpr FloatRegister ScratchSimd128Reg = InvalidFloatReg;

// StackPointer is intentionally undefined on ARM64 to prevent misuse:
//  using sp as a base register is only valid if sp % 16 == 0.
static constexpr Register RealStackPointer = { Registers::sp };

static constexpr Register PseudoStackPointer = { Registers::x28 };
static constexpr ARMRegister PseudoStackPointer64 = { Registers::x28, 64 };
static constexpr ARMRegister PseudoStackPointer32 = { Registers::x28, 32 };

// StackPointer for use by irregexp.
static constexpr Register RegExpStackPointer = PseudoStackPointer;

static constexpr Register IntArgReg0 = { Registers::x0 };
static constexpr Register IntArgReg1 = { Registers::x1 };
static constexpr Register IntArgReg2 = { Registers::x2 };
static constexpr Register IntArgReg3 = { Registers::x3 };
static constexpr Register IntArgReg4 = { Registers::x4 };
static constexpr Register IntArgReg5 = { Registers::x5 };
static constexpr Register IntArgReg6 = { Registers::x6 };
static constexpr Register IntArgReg7 = { Registers::x7 };
static constexpr Register GlobalReg =  { Registers::x20 };
static constexpr Register HeapReg = { Registers::x21 };
static constexpr Register HeapLenReg = { Registers::x22 };

// Define unsized Registers.
#define DEFINE_UNSIZED_REGISTERS(N)  \
static constexpr Register r##N = { Registers::x##N };
REGISTER_CODE_LIST(DEFINE_UNSIZED_REGISTERS)
#undef DEFINE_UNSIZED_REGISTERS
static constexpr Register ip0 = { Registers::x16 };
static constexpr Register ip1 = { Registers::x16 };
static constexpr Register fp  = { Registers::x30 };
static constexpr Register lr  = { Registers::x30 };
static constexpr Register rzr = { Registers::xzr };

// Import VIXL registers into the js::jit namespace.
#define IMPORT_VIXL_REGISTERS(N)  \
static constexpr ARMRegister w##N = vixl::w##N;  \
static constexpr ARMRegister x##N = vixl::x##N;
REGISTER_CODE_LIST(IMPORT_VIXL_REGISTERS)
#undef IMPORT_VIXL_REGISTERS
static constexpr ARMRegister wzr = vixl::wzr;
static constexpr ARMRegister xzr = vixl::xzr;
static constexpr ARMRegister wsp = vixl::wsp;
static constexpr ARMRegister sp = vixl::sp;

// Import VIXL VRegisters into the js::jit namespace.
#define IMPORT_VIXL_VREGISTERS(N)  \
static constexpr ARMFPRegister s##N = vixl::s##N;  \
static constexpr ARMFPRegister d##N = vixl::d##N;
REGISTER_CODE_LIST(IMPORT_VIXL_VREGISTERS)
#undef IMPORT_VIXL_VREGISTERS

static constexpr ValueOperand JSReturnOperand = ValueOperand(JSReturnReg);

// Registers used in the GenerateFFIIonExit Enable Activation block.
static constexpr Register AsmJSIonExitRegCallee = r8;
static constexpr Register AsmJSIonExitRegE0 = r0;
static constexpr Register AsmJSIonExitRegE1 = r1;
static constexpr Register AsmJSIonExitRegE2 = r2;
static constexpr Register AsmJSIonExitRegE3 = r3;

// Registers used in the GenerateFFIIonExit Disable Activation block.
// None of these may be the second scratch register.
static constexpr Register AsmJSIonExitRegReturnData = r2;
static constexpr Register AsmJSIonExitRegReturnType = r3;
static constexpr Register AsmJSIonExitRegD0 = r0;
static constexpr Register AsmJSIonExitRegD1 = r1;
static constexpr Register AsmJSIonExitRegD2 = r4;

static constexpr Register JSReturnReg_Type = r3;
static constexpr Register JSReturnReg_Data = r2;

static constexpr FloatRegister NANReg = { FloatRegisters::d14, FloatRegisters::Single };
// N.B. r8 isn't listed as an aapcs temp register, but we can use it as such because we never
// use return-structs.
static constexpr Register CallTempNonArgRegs[] = { r8, r9, r10, r11, r12, r13, r14, r15 };
static const uint32_t NumCallTempNonArgRegs =
    mozilla::ArrayLength(CallTempNonArgRegs);

static constexpr uint32_t JitStackAlignment = 16;

static constexpr uint32_t JitStackValueAlignment = JitStackAlignment / sizeof(Value);
static_assert(JitStackAlignment % sizeof(Value) == 0 && JitStackValueAlignment >= 1,
  "Stack alignment should be a non-zero multiple of sizeof(Value)");

// This boolean indicates whether we support SIMD instructions flavoured for
// this architecture or not. Rather than a method in the LIRGenerator, it is
// here such that it is accessible from the entire codebase. Once full support
// for SIMD is reached on all tier-1 platforms, this constant can be deleted.
static constexpr bool SupportsSimd = false;
static constexpr uint32_t SimdMemoryAlignment = 16;

static_assert(CodeAlignment % SimdMemoryAlignment == 0,
  "Code alignment should be larger than any of the alignments which are used for "
  "the constant sections of the code buffer.  Thus it should be larger than the "
  "alignment for SIMD constants.");

static const uint32_t AsmJSStackAlignment = SimdMemoryAlignment;
static const int32_t AsmJSGlobalRegBias = 1024;

class Assembler : public vixl::Assembler
{
  public:
    Assembler()
      : vixl::Assembler()
    { }

    typedef vixl::Condition Condition;

    void finish();
    bool asmMergeWith(const Assembler& other) {
        MOZ_CRASH("NYI");
    }
    void trace(JSTracer* trc);

    // Emit the jump table, returning the BufferOffset to the first entry in the table.
    BufferOffset emitExtendedJumpTable();
    BufferOffset ExtendedJumpTable_;
    void executableCopy(uint8_t* buffer);

    BufferOffset immPool(ARMRegister dest, uint8_t* value, vixl::LoadLiteralOp op,
                         ARMBuffer::PoolEntry* pe = nullptr);
    BufferOffset immPool64(ARMRegister dest, uint64_t value, ARMBuffer::PoolEntry* pe = nullptr);
    BufferOffset immPool64Branch(RepatchLabel* label, ARMBuffer::PoolEntry* pe, vixl::Condition c);
    BufferOffset fImmPool(ARMFPRegister dest, uint8_t* value, vixl::LoadLiteralOp op);
    BufferOffset fImmPool64(ARMFPRegister dest, double value);
    BufferOffset fImmPool32(ARMFPRegister dest, float value);

    void bind(Label* label) { bind(label, nextOffset()); }
    void bind(Label* label, BufferOffset boff);
    void bind(RepatchLabel* label);

    bool oom() const {
        return AssemblerShared::oom() ||
            armbuffer_.oom() ||
            jumpRelocations_.oom() ||
            dataRelocations_.oom() ||
            preBarriers_.oom();
    }

    void copyJumpRelocationTable(uint8_t* dest) const {
        if (jumpRelocations_.length())
            memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
    }
    void copyDataRelocationTable(uint8_t* dest) const {
        if (dataRelocations_.length())
            memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
    }
    void copyPreBarrierTable(uint8_t* dest) const {
        if (preBarriers_.length())
            memcpy(dest, preBarriers_.buffer(), preBarriers_.length());
    }

    size_t jumpRelocationTableBytes() const {
        return jumpRelocations_.length();
    }
    size_t dataRelocationTableBytes() const {
        return dataRelocations_.length();
    }
    size_t preBarrierTableBytes() const {
        return preBarriers_.length();
    }
    size_t bytesNeeded() const {
        return SizeOfCodeGenerated() +
            jumpRelocationTableBytes() +
            dataRelocationTableBytes() +
            preBarrierTableBytes();
    }

    void processCodeLabels(uint8_t* rawCode) {
        for (size_t i = 0; i < codeLabels_.length(); i++) {
            CodeLabel label = codeLabels_[i];
            Bind(rawCode, label.patchAt(), rawCode + label.target()->offset());
        }
    }

    void Bind(uint8_t* rawCode, CodeOffset* label, const void* address) {
        *reinterpret_cast<const void**>(rawCode + label->offset()) = address;
    }

    void retarget(Label* cur, Label* next);
    void retargetWithOffset(size_t baseOffset, const LabelBase* label, LabelBase* target) {
        MOZ_CRASH("NYI");
    }

    // The buffer is about to be linked. Ensure any constant pools or
    // excess bookkeeping has been flushed to the instruction stream.
    void flush() {
        armbuffer_.flushPool();
    }

    int actualIndex(int curOffset) {
        ARMBuffer::PoolEntry pe(curOffset);
        return armbuffer_.poolEntryOffset(pe);
    }
    size_t labelToPatchOffset(CodeOffset label) {
        return label.offset();
    }
    static uint8_t* PatchableJumpAddress(JitCode* code, uint32_t index) {
        return code->raw() + index;
    }
    void setPrinter(Sprinter* sp) {
    }

    static bool SupportsFloatingPoint() { return true; }
    static bool SupportsSimd() { return js::jit::SupportsSimd; }

    // Tracks a jump that is patchable after finalization.
    void addJumpRelocation(BufferOffset src, Relocation::Kind reloc);

  protected:
    // Add a jump whose target is unknown until finalization.
    // The jump may not be patched at runtime.
    void addPendingJump(BufferOffset src, ImmPtr target, Relocation::Kind kind);

    // Add a jump whose target is unknown until finalization, and may change
    // thereafter. The jump is patchable at runtime.
    size_t addPatchableJump(BufferOffset src, Relocation::Kind kind);

  public:
    static uint32_t PatchWrite_NearCallSize() {
        return 4;
    }

    static uint32_t NopSize() {
        return 4;
    }

    static void PatchWrite_NearCall(CodeLocationLabel start, CodeLocationLabel toCall) {
        Instruction* dest = (Instruction*)start.raw();
        //printf("patching %p with call to %p\n", start.raw(), toCall.raw());
        bl(dest, ((Instruction*)toCall.raw() - dest)>>2);

    }
    static void PatchDataWithValueCheck(CodeLocationLabel label,
                                        PatchedImmPtr newValue,
                                        PatchedImmPtr expected);

    static void PatchDataWithValueCheck(CodeLocationLabel label,
                                        ImmPtr newValue,
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

    static void TraceJumpRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);
    static void TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader);

    static void PatchInstructionImmediate(uint8_t* code, PatchedImmPtr imm);

    static void FixupNurseryObjects(JSContext* cx, JitCode* code, CompactBufferReader& reader,
                                    const ObjectVector& nurseryObjects);

  public:
    // A Jump table entry is 2 instructions, with 8 bytes of raw data
    static const size_t SizeOfJumpTableEntry = 16;

    struct JumpTableEntry
    {
        uint32_t ldr;
        uint32_t br;
        void* data;

        Instruction* getLdr() {
            return reinterpret_cast<Instruction*>(&ldr);
        }
    };

    // Offset of the patchable target for the given entry.
    static const size_t OffsetOfJumpTableEntryPointer = 8;

  public:
    static void UpdateBoundsCheck(uint32_t logHeapSize, Instruction* inst);

    void writeCodePointer(AbsoluteLabel* absoluteLabel) {
        MOZ_ASSERT(!absoluteLabel->bound());
        uintptr_t x = LabelBase::INVALID_OFFSET;
        BufferOffset off = EmitData(&x, sizeof(uintptr_t));

        // The x86/x64 makes general use of AbsoluteLabel and weaves a linked list
        // of uses of an AbsoluteLabel through the assembly. ARM only uses labels
        // for the case statements of switch jump tables. Thus, for simplicity, we
        // simply treat the AbsoluteLabel as a label and bind it to the offset of
        // the jump table entry that needs to be patched.
        LabelBase* label = absoluteLabel;
        label->bind(off.getOffset());
    }

    void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                     const Disassembler::HeapAccess& heapAccess)
    {
        MOZ_CRASH("verifyHeapAccessDisassembly");
    }

  protected:
    // Because jumps may be relocated to a target inaccessible by a short jump,
    // each relocatable jump must have a unique entry in the extended jump table.
    // Valid relocatable targets are of type Relocation::JITCODE.
    struct JumpRelocation
    {
        BufferOffset jump; // Offset to the short jump, from the start of the code buffer.
        uint32_t extendedTableIndex; // Unique index within the extended jump table.

        JumpRelocation(BufferOffset jump, uint32_t extendedTableIndex)
          : jump(jump), extendedTableIndex(extendedTableIndex)
        { }
    };

    // Structure for fixing up pc-relative loads/jumps when the machine
    // code gets moved (executable copy, gc, etc.).
    struct RelativePatch
    {
        BufferOffset offset;
        void* target;
        Relocation::Kind kind;

        RelativePatch(BufferOffset offset, void* target, Relocation::Kind kind)
          : offset(offset), target(target), kind(kind)
        { }
    };

    // List of jumps for which the target is either unknown until finalization,
    // or cannot be known due to GC. Each entry here requires a unique entry
    // in the extended jump table, and is patched at finalization.
    js::Vector<RelativePatch, 8, SystemAllocPolicy> pendingJumps_;

    // Final output formatters.
    CompactBufferWriter jumpRelocations_;
    CompactBufferWriter dataRelocations_;
    CompactBufferWriter preBarriers_;
};

static const uint32_t NumIntArgRegs = 8;
static const uint32_t NumFloatArgRegs = 8;

class ABIArgGenerator
{
  public:
    ABIArgGenerator()
      : intRegIndex_(0),
        floatRegIndex_(0),
        stackOffset_(0),
        current_()
    { }

    ABIArg next(MIRType argType);
    ABIArg& current() { return current_; }
    uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }

  public:
    static const Register NonArgReturnReg0;
    static const Register NonArgReturnReg1;
    static const Register NonVolatileReg;
    static const Register NonArg_VolatileReg;
    static const Register NonReturn_VolatileReg0;
    static const Register NonReturn_VolatileReg1;

  protected:
    unsigned intRegIndex_;
    unsigned floatRegIndex_;
    uint32_t stackOffset_;
    ABIArg current_;
};

static inline bool
GetIntArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out)
{
    if (usedIntArgs >= NumIntArgRegs)
        return false;
    *out = Register::FromCode(usedIntArgs);
    return true;
}

static inline bool
GetFloatArgReg(uint32_t usedIntArgs, uint32_t usedFloatArgs, FloatRegister* out)
{
    if (usedFloatArgs >= NumFloatArgRegs)
        return false;
    *out = FloatRegister::FromCode(usedFloatArgs);
    return true;
}

// Get a register in which we plan to put a quantity that will be used as an
// integer argument.  This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool
GetTempRegForIntArg(uint32_t usedIntArgs, uint32_t usedFloatArgs, Register* out)
{
    if (GetIntArgReg(usedIntArgs, usedFloatArgs, out))
        return true;
    // Unfortunately, we have to assume things about the point at which
    // GetIntArgReg returns false, because we need to know how many registers it
    // can allocate.
    usedIntArgs -= NumIntArgRegs;
    if (usedIntArgs >= NumCallTempNonArgRegs)
        return false;
    *out = CallTempNonArgRegs[usedIntArgs];
    return true;
}

void PatchJump(CodeLocationJump& jump_, CodeLocationLabel label,
               ReprotectCode reprotect = DontReprotect);

static inline void
PatchBackedge(CodeLocationJump& jump_, CodeLocationLabel label, JitRuntime::BackedgeTarget target)
{
    PatchJump(jump_, label);
}

// Forbids pool generation during a specified interval. Not nestable.
class AutoForbidPools
{
    Assembler* asm_;

  public:
    AutoForbidPools(Assembler* asm_, size_t maxInst)
      : asm_(asm_)
    {
        asm_->enterNoPool(maxInst);
    }

    ~AutoForbidPools() {
        asm_->leaveNoPool();
    }
};

} // namespace jit
} // namespace js

#endif // A64_ASSEMBLER_A64_H_
