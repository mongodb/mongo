/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Copyright (c) 1994-2006 Sun Microsystems Inc.
// All Rights Reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// - Redistributions of source code must retain the above copyright notice,
// this list of conditions and the following disclaimer.
//
// - Redistribution in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// - Neither the name of Sun Microsystems or the names of contributors may
// be used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// The original source code covered by the above license above has been
// modified significantly by Google Inc.
// Copyright 2021 the V8 project authors. All rights reserved.

#ifndef jit_riscv64_Assembler_riscv64_h
#define jit_riscv64_Assembler_riscv64_h

#include "mozilla/Assertions.h"
#include "mozilla/Sprintf.h"

#include <stdint.h>

#include "jit/CompactBuffer.h"
#include "jit/JitCode.h"
#include "jit/JitSpewer.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#include "jit/riscv64/Architecture-riscv64.h"
#include "jit/riscv64/constant/Constant-riscv64.h"
#include "jit/riscv64/extension/base-assembler-riscv.h"
#include "jit/riscv64/extension/base-riscv-i.h"
#include "jit/riscv64/extension/extension-riscv-a.h"
#include "jit/riscv64/extension/extension-riscv-c.h"
#include "jit/riscv64/extension/extension-riscv-d.h"
#include "jit/riscv64/extension/extension-riscv-f.h"
#include "jit/riscv64/extension/extension-riscv-m.h"
#include "jit/riscv64/extension/extension-riscv-v.h"
#include "jit/riscv64/extension/extension-riscv-zicsr.h"
#include "jit/riscv64/extension/extension-riscv-zifencei.h"
#include "jit/riscv64/Register-riscv64.h"
#include "jit/shared/Assembler-shared.h"
#include "jit/shared/Disassembler-shared.h"
#include "jit/shared/IonAssemblerBufferWithConstantPools.h"
#include "js/HashTable.h"
#include "wasm/WasmTypeDecls.h"
namespace js {
namespace jit {

struct ScratchFloat32Scope : public AutoFloatRegisterScope {
  explicit ScratchFloat32Scope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchFloat32Reg) {}
};

struct ScratchDoubleScope : public AutoFloatRegisterScope {
  explicit ScratchDoubleScope(MacroAssembler& masm)
      : AutoFloatRegisterScope(masm, ScratchDoubleReg) {}
};

struct ScratchRegisterScope : public AutoRegisterScope {
  explicit ScratchRegisterScope(MacroAssembler& masm)
      : AutoRegisterScope(masm, ScratchRegister) {}
};

class MacroAssembler;

inline Imm32 Imm64::secondHalf() const { return hi(); }
inline Imm32 Imm64::firstHalf() const { return low(); }

static constexpr uint32_t ABIStackAlignment = 8;
static constexpr uint32_t CodeAlignment = 16;
static constexpr uint32_t JitStackAlignment = 8;
static constexpr uint32_t JitStackValueAlignment =
    JitStackAlignment / sizeof(Value);
static const uint32_t WasmStackAlignment = 16;
static const uint32_t WasmTrapInstructionLength = 2 * sizeof(uint32_t);
// See comments in wasm::GenerateFunctionPrologue.  The difference between these
// is the size of the largest callable prologue on the platform.
static constexpr uint32_t WasmCheckedCallEntryOffset = 0u;
static constexpr uint32_t WasmCheckedTailEntryOffset = 20u;

static const Scale ScalePointer = TimesEight;

class Assembler;

static constexpr int32_t SliceSize = 1024;

typedef js::jit::AssemblerBufferWithConstantPools<
    SliceSize, 4, Instruction, Assembler, NumShortBranchRangeTypes>
    Buffer;

class Assembler : public AssemblerShared,
                  public AssemblerRISCVI,
                  public AssemblerRISCVA,
                  public AssemblerRISCVF,
                  public AssemblerRISCVD,
                  public AssemblerRISCVM,
                  public AssemblerRISCVC,
                  public AssemblerRISCVZicsr,
                  public AssemblerRISCVZifencei {
  GeneralRegisterSet scratch_register_list_;

  static constexpr int kInvalidSlotPos = -1;

#ifdef JS_JITSPEW
  Sprinter* printer;
#endif
  bool enoughLabelCache_ = true;

 protected:
  using LabelOffset = int32_t;
  using LabelCahe =
      HashMap<LabelOffset, BufferOffset, js::DefaultHasher<LabelOffset>,
              js::SystemAllocPolicy>;
  LabelCahe label_cache_;
  void NoEnoughLabelCache() { enoughLabelCache_ = false; }
  CompactBufferWriter jumpRelocations_;
  CompactBufferWriter dataRelocations_;
  Buffer m_buffer;
  bool isFinished = false;
  Instruction* editSrc(BufferOffset bo) { return m_buffer.getInst(bo); }

  struct RelativePatch {
    // the offset within the code buffer where the value is loaded that
    // we want to fix-up
    BufferOffset offset;
    void* target;
    RelocationKind kind;

    RelativePatch(BufferOffset offset, void* target, RelocationKind kind)
        : offset(offset), target(target), kind(kind) {}
  };

  js::Vector<RelativePatch, 8, SystemAllocPolicy> jumps_;

  void addPendingJump(BufferOffset src, ImmPtr target, RelocationKind kind) {
    enoughMemory_ &= jumps_.append(RelativePatch(src, target.value, kind));
    if (kind == RelocationKind::JITCODE) {
      jumpRelocations_.writeUnsigned(src.getOffset());
    }
  }

  void addLongJump(BufferOffset src, BufferOffset dst) {
    CodeLabel cl;
    cl.patchAt()->bind(src.getOffset());
    cl.target()->bind(dst.getOffset());
    cl.setLinkMode(CodeLabel::JumpImmediate);
    addCodeLabel(std::move(cl));
  }

 public:
  static bool FLAG_riscv_debug;

  Assembler()
      : scratch_register_list_((1 << t5.code()) | (1 << t4.code()) |
                               (1 << t6.code())),
#ifdef JS_JITSPEW
        printer(nullptr),
#endif
        m_buffer(/*guardSize*/ 2, /*headerSize*/ 2, /*instBufferAlign*/ 8,
                 /*poolMaxOffset*/ GetPoolMaxOffset(), /*pcBias*/ 8,
                 /*alignFillInst*/ kNopByte, /*nopFillInst*/ kNopByte),
        isFinished(false) {
  }
  static uint32_t NopFill;
  static uint32_t AsmPoolMaxOffset;
  static uint32_t GetPoolMaxOffset();
  bool reserve(size_t size);
  bool oom() const;
  void setPrinter(Sprinter* sp) {
#ifdef JS_JITSPEW
    printer = sp;
#endif
  }
  void finish() {
    MOZ_ASSERT(!isFinished);
    isFinished = true;
  }
  void enterNoPool(size_t maxInst) { m_buffer.enterNoPool(maxInst); }
  void leaveNoPool() { m_buffer.leaveNoPool(); }
  bool swapBuffer(wasm::Bytes& bytes);
  // Size of the instruction stream, in bytes.
  size_t size() const;
  // Size of the data table, in bytes.
  size_t bytesNeeded() const;
  // Size of the jump relocation table, in bytes.
  size_t jumpRelocationTableBytes() const;
  size_t dataRelocationTableBytes() const;
  void copyJumpRelocationTable(uint8_t* dest);
  void copyDataRelocationTable(uint8_t* dest);
  // Copy the assembly code to the given buffer, and perform any pending
  // relocations relying on the target address.
  void executableCopy(uint8_t* buffer);
  // API for speaking with the IonAssemblerBufferWithConstantPools generate an
  // initial placeholder instruction that we want to later fix up.
  static void InsertIndexIntoTag(uint8_t* load, uint32_t index);
  static void PatchConstantPoolLoad(void* loadAddr, void* constPoolAddr);
  // We're not tracking short-range branches for ARM for now.
  static void PatchShortRangeBranchToVeneer(Buffer*, unsigned rangeIdx,
                                            BufferOffset deadline,
                                            BufferOffset veneer);
  struct PoolHeader {
    uint32_t data;

    struct Header {
      // The size should take into account the pool header.
      // The size is in units of Instruction (4bytes), not byte.
      union {
        struct {
          uint32_t size : 15;

          // "Natural" guards are part of the normal instruction stream,
          // while "non-natural" guards are inserted for the sole purpose
          // of skipping around a pool.
          uint32_t isNatural : 1;
          uint32_t ONES : 16;
        };
        uint32_t data;
      };

      Header(int size_, bool isNatural_)
          : size(size_), isNatural(isNatural_), ONES(0xffff) {}

      Header(uint32_t data) : data(data) {
        static_assert(sizeof(Header) == sizeof(uint32_t));
        MOZ_ASSERT(ONES == 0xffff);
      }

      uint32_t raw() const {
        static_assert(sizeof(Header) == sizeof(uint32_t));
        return data;
      }
    };

    PoolHeader(int size_, bool isNatural_)
        : data(Header(size_, isNatural_).raw()) {}

    uint32_t size() const {
      Header tmp(data);
      return tmp.size;
    }

    uint32_t isNatural() const {
      Header tmp(data);
      return tmp.isNatural;
    }
  };

  static void WritePoolHeader(uint8_t* start, Pool* p, bool isNatural);
  static void WritePoolGuard(BufferOffset branch, Instruction* inst,
                             BufferOffset dest);
  void processCodeLabels(uint8_t* rawCode);
  BufferOffset nextOffset() { return m_buffer.nextOffset(); }
  // Get the buffer offset of the next inserted instruction. This may flush
  // constant pools.
  BufferOffset nextInstrOffset(int numInstr = 1) {
    return m_buffer.nextInstrOffset(numInstr);
  }
  void comment(const char* msg) { spew("; %s", msg); }

#ifdef JS_JITSPEW
  inline void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3) {
    if (MOZ_UNLIKELY(printer || JitSpewEnabled(JitSpew_Codegen))) {
      va_list va;
      va_start(va, fmt);
      spew(fmt, va);
      va_end(va);
    }
  }

#else
  MOZ_ALWAYS_INLINE void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3) {}
#endif

#ifdef JS_JITSPEW
  MOZ_COLD void spew(const char* fmt, va_list va) MOZ_FORMAT_PRINTF(2, 0) {
    // Buffer to hold the formatted string. Note that this may contain
    // '%' characters, so do not pass it directly to printf functions.
    char buf[200];

    int i = VsprintfLiteral(buf, fmt, va);
    if (i > -1) {
      if (printer) {
        printer->printf("%s\n", buf);
      }
      js::jit::JitSpew(js::jit::JitSpew_Codegen, "%s", buf);
    }
  }
#endif

  enum Condition {
    Overflow = overflow,
    Below = Uless,
    BelowOrEqual = Uless_equal,
    Above = Ugreater,
    AboveOrEqual = Ugreater_equal,
    Equal = equal,
    NotEqual = not_equal,
    GreaterThan = greater,
    GreaterThanOrEqual = greater_equal,
    LessThan = less,
    LessThanOrEqual = less_equal,
    Always = cc_always,
    CarrySet,
    CarryClear,
    Signed,
    NotSigned,
    Zero,
    NonZero,
  };

  enum DoubleCondition {
    // These conditions will only evaluate to true if the comparison is ordered
    // - i.e. neither operand is NaN.
    DoubleOrdered,
    DoubleEqual,
    DoubleNotEqual,
    DoubleGreaterThan,
    DoubleGreaterThanOrEqual,
    DoubleLessThan,
    DoubleLessThanOrEqual,
    // If either operand is NaN, these conditions always evaluate to true.
    DoubleUnordered,
    DoubleEqualOrUnordered,
    DoubleNotEqualOrUnordered,
    DoubleGreaterThanOrUnordered,
    DoubleGreaterThanOrEqualOrUnordered,
    DoubleLessThanOrUnordered,
    DoubleLessThanOrEqualOrUnordered,
    FIRST_UNORDERED = DoubleUnordered,
    LAST_UNORDERED = DoubleLessThanOrEqualOrUnordered
  };

  Register getStackPointer() const { return StackPointer; }
  void flushBuffer() {}
  static int disassembleInstr(Instr instr, bool enable_spew = false);
  int target_at(BufferOffset pos, bool is_internal);
  static int target_at(Instruction* instruction, BufferOffset pos,
                       bool is_internal, Instruction* instruction2 = nullptr);
  uint32_t next_link(Label* label, bool is_internal);
  static uintptr_t target_address_at(Instruction* pos);
  static void set_target_value_at(Instruction* pc, uint64_t target);
  void target_at_put(BufferOffset pos, BufferOffset target_pos,
                     bool trampoline = false);
  virtual int32_t branch_offset_helper(Label* L, OffsetSize bits);
  int32_t branch_long_offset(Label* L);

  // Determines if Label is bound and near enough so that branch instruction
  // can be used to reach it, instead of jump instruction.
  bool is_near(Label* L);
  bool is_near(Label* L, OffsetSize bits);
  bool is_near_branch(Label* L);

  void nopAlign(int m) {
    MOZ_ASSERT(m >= 4 && (m & (m - 1)) == 0);
    while ((currentOffset() & (m - 1)) != 0) {
      nop();
    }
  }
  virtual void emit(Instr x) {
    MOZ_ASSERT(hasCreator());
    m_buffer.putInt(x);
#ifdef DEBUG
    if (!oom()) {
      DEBUG_PRINTF(
          "0x%lx(%lx):",
          (uint64_t)editSrc(BufferOffset(currentOffset() - sizeof(Instr))),
          currentOffset() - sizeof(Instr));
      disassembleInstr(x, JitSpewEnabled(JitSpew_Codegen));
    }
#endif
  }
  virtual void emit(ShortInstr x) { MOZ_CRASH(); }
  virtual void emit(uint64_t x) { MOZ_CRASH(); }
  virtual void emit(uint32_t x) {
    DEBUG_PRINTF(
        "0x%lx(%lx): uint32_t: %d\n",
        (uint64_t)editSrc(BufferOffset(currentOffset() - sizeof(Instr))),
        currentOffset() - sizeof(Instr), x);
    m_buffer.putInt(x);
  }

  void instr_at_put(BufferOffset offset, Instr instr) {
    DEBUG_PRINTF("\t[instr_at_put\n");
    DEBUG_PRINTF("\t%p %d \n\t", editSrc(offset), offset.getOffset());
    disassembleInstr(editSrc(offset)->InstructionBits());
    DEBUG_PRINTF("\t");
    *reinterpret_cast<Instr*>(editSrc(offset)) = instr;
    disassembleInstr(editSrc(offset)->InstructionBits());
    DEBUG_PRINTF("\t]\n");
  }

  static Condition InvertCondition(Condition);

  static DoubleCondition InvertCondition(DoubleCondition);

  static uint64_t ExtractLoad64Value(Instruction* inst0);
  static void UpdateLoad64Value(Instruction* inst0, uint64_t value);
  static void PatchDataWithValueCheck(CodeLocationLabel label, ImmPtr newValue,
                                      ImmPtr expectedValue);
  static void PatchDataWithValueCheck(CodeLocationLabel label,
                                      PatchedImmPtr newValue,
                                      PatchedImmPtr expectedValue);
  static void PatchWrite_Imm32(CodeLocationLabel label, Imm32 imm);

  static void PatchWrite_NearCall(CodeLocationLabel start,
                                  CodeLocationLabel toCall) {
    Instruction* inst = (Instruction*)start.raw();
    uint8_t* dest = toCall.raw();

    // Overwrite whatever instruction used to be here with a call.
    // Always use long jump for two reasons:
    // - Jump has to be the same size because of PatchWrite_NearCallSize.
    // - Return address has to be at the end of replaced block.
    // Short jump wouldn't be more efficient.
    // WriteLoad64Instructions will emit 6 instrs to load a addr.
    Assembler::WriteLoad64Instructions(inst, ScratchRegister, (uint64_t)dest);
    Instr jalr_ = JALR | (ra.code() << kRdShift) | (0x0 << kFunct3Shift) |
                  (ScratchRegister.code() << kRs1Shift) | (0x0 << kImm12Shift);
    *reinterpret_cast<Instr*>(inst + 6 * kInstrSize) = jalr_;
  }
  static void WriteLoad64Instructions(Instruction* inst0, Register reg,
                                      uint64_t value);

  static uint32_t PatchWrite_NearCallSize() { return 7 * sizeof(uint32_t); }

  static void TraceJumpRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);
  static void TraceDataRelocations(JSTracer* trc, JitCode* code,
                                   CompactBufferReader& reader);

  static void ToggleToJmp(CodeLocationLabel inst_);
  static void ToggleToCmp(CodeLocationLabel inst_);
  static void ToggleCall(CodeLocationLabel inst_, bool enable);

  static void Bind(uint8_t* rawCode, const CodeLabel& label);
  // label operations
  void bind(Label* label, BufferOffset boff = BufferOffset());
  void bind(CodeLabel* label) { label->target()->bind(currentOffset()); }
  uint32_t currentOffset() { return nextOffset().getOffset(); }
  void retarget(Label* label, Label* target);
  static uint32_t NopSize() { return 4; }

  static uintptr_t GetPointer(uint8_t* instPtr) {
    Instruction* inst = (Instruction*)instPtr;
    return Assembler::ExtractLoad64Value(inst);
  }

  static bool HasRoundInstruction(RoundingMode) { return false; }

  void verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                   const Disassembler::HeapAccess& heapAccess) {
    MOZ_CRASH();
  }

  void setUnlimitedBuffer() { m_buffer.setUnlimited(); }

  GeneralRegisterSet* GetScratchRegisterList() {
    return &scratch_register_list_;
  }

  void EmitConstPoolWithJumpIfNeeded(size_t margin = 0) {}

  // As opposed to x86/x64 version, the data relocation has to be executed
  // before to recover the pointer, and not after.
  void writeDataRelocation(ImmGCPtr ptr) {
    // Raw GC pointer relocations and Value relocations both end up in
    // TraceOneDataRelocation.
    if (ptr.value) {
      if (gc::IsInsideNursery(ptr.value)) {
        embedsNurseryPointers_ = true;
      }
      dataRelocations_.writeUnsigned(nextOffset().getOffset());
    }
  }

  bool appendRawCode(const uint8_t* code, size_t numBytes);

  void assertNoGCThings() const {
#ifdef DEBUG
    MOZ_ASSERT(dataRelocations_.length() == 0);
    for (auto& j : jumps_) {
      MOZ_ASSERT(j.kind == RelocationKind::HARDCODED);
    }
#endif
  }

  // Assembler Pseudo Instructions (Tables 25.2, 25.3, RISC-V Unprivileged ISA)
  void break_(uint32_t code, bool break_as_stop = false);
  void nop();
  void RV_li(Register rd, intptr_t imm);
  static int RV_li_count(int64_t imm, bool is_get_temp_reg = false);
  void GeneralLi(Register rd, int64_t imm);
  static int GeneralLiCount(intptr_t imm, bool is_get_temp_reg = false);
  void RecursiveLiImpl(Register rd, intptr_t imm);
  void RecursiveLi(Register rd, intptr_t imm);
  static int RecursiveLiCount(intptr_t imm);
  static int RecursiveLiImplCount(intptr_t imm);
  // Returns the number of instructions required to load the immediate
  static int li_estimate(intptr_t imm, bool is_get_temp_reg = false);
  // Loads an immediate, always using 8 instructions, regardless of the value,
  // so that it can be modified later.
  void li_constant(Register rd, intptr_t imm);
  void li_ptr(Register rd, intptr_t imm);
};

class ABIArgGenerator {
 public:
  ABIArgGenerator()
      : intRegIndex_(0), floatRegIndex_(0), stackOffset_(0), current_() {}
  ABIArg next(MIRType);
  ABIArg& current() { return current_; }
  uint32_t stackBytesConsumedSoFar() const { return stackOffset_; }
  void increaseStackOffset(uint32_t bytes) { stackOffset_ += bytes; }

 protected:
  unsigned intRegIndex_;
  unsigned floatRegIndex_;
  uint32_t stackOffset_;
  ABIArg current_;
};

// Note that nested uses of these are allowed, but the inner calls must imply
// an area of code which exists only inside the area of code implied by the
// outermost call.  Otherwise AssemblerBufferWithConstantPools::enterNoPool
// will assert.
class BlockTrampolinePoolScope {
 public:
  explicit BlockTrampolinePoolScope(Assembler* assem, int margin)
      : assem_(assem) {
    assem_->enterNoPool(margin);
  }
  ~BlockTrampolinePoolScope() { assem_->leaveNoPool(); }

 private:
  Assembler* assem_;
  BlockTrampolinePoolScope() = delete;
  BlockTrampolinePoolScope(const BlockTrampolinePoolScope&) = delete;
  BlockTrampolinePoolScope& operator=(const BlockTrampolinePoolScope&) = delete;
};

class UseScratchRegisterScope {
 public:
  explicit UseScratchRegisterScope(Assembler* assembler);
  ~UseScratchRegisterScope();

  Register Acquire();
  bool hasAvailable() const;
  void Include(const GeneralRegisterSet& list) {
    *available_ = GeneralRegisterSet::Intersect(*available_, list);
  }
  void Exclude(const GeneralRegisterSet& list) {
    *available_ = GeneralRegisterSet::Subtract(*available_, list);
  }

 private:
  GeneralRegisterSet* available_;
  GeneralRegisterSet old_available_;
};

// Class Operand represents a shifter operand in data processing instructions.
class Operand {
 public:
  enum Tag { REG, FREG, MEM, IMM };
  Operand(FloatRegister freg) : tag(FREG), rm_(freg.code()) {}

  explicit Operand(Register base, Imm32 off)
      : tag(MEM), rm_(base.code()), offset_(off.value) {}

  explicit Operand(Register base, int32_t off)
      : tag(MEM), rm_(base.code()), offset_(off) {}

  explicit Operand(const Address& addr)
      : tag(MEM), rm_(addr.base.code()), offset_(addr.offset) {}

  explicit Operand(intptr_t immediate) : tag(IMM), rm_() { value_ = immediate; }
  // Register.
  Operand(const Register rm) : tag(REG), rm_(rm.code()) {}
  // Return true if this is a register operand.
  bool is_reg() const { return tag == REG; }
  bool is_freg() const { return tag == FREG; }
  bool is_mem() const { return tag == MEM; }
  bool is_imm() const { return tag == IMM; }
  inline intptr_t immediate() const {
    MOZ_ASSERT(is_imm());
    return value_;
  }
  bool IsImmediate() const { return !is_reg(); }
  Register rm() const { return Register::FromCode(rm_); }
  int32_t offset() const {
    MOZ_ASSERT(is_mem());
    return offset_;
  }

  FloatRegister toFReg() const {
    MOZ_ASSERT(tag == FREG);
    return FloatRegister::FromCode(rm_);
  }

  Register toReg() const {
    MOZ_ASSERT(tag == REG);
    return Register::FromCode(rm_);
  }

  Address toAddress() const {
    MOZ_ASSERT(tag == MEM);
    return Address(Register::FromCode(rm_), offset());
  }

 private:
  Tag tag;
  uint32_t rm_;
  int32_t offset_;
  intptr_t value_;  // valid if rm_ == no_reg

  friend class Assembler;
  friend class MacroAssembler;
};

static const uint32_t NumIntArgRegs = 8;
static const uint32_t NumFloatArgRegs = 8;
static inline bool GetIntArgReg(uint32_t usedIntArgs, Register* out) {
  if (usedIntArgs < NumIntArgRegs) {
    *out = Register::FromCode(a0.code() + usedIntArgs);
    return true;
  }
  return false;
}

static inline bool GetFloatArgReg(uint32_t usedFloatArgs, FloatRegister* out) {
  if (usedFloatArgs < NumFloatArgRegs) {
    *out = FloatRegister::FromCode(fa0.code() + usedFloatArgs);
    return true;
  }
  return false;
}

// Get a register in which we plan to put a quantity that will be used as an
// integer argument. This differs from GetIntArgReg in that if we have no more
// actual argument registers to use we will fall back on using whatever
// CallTempReg* don't overlap the argument registers, and only fail once those
// run out too.
static inline bool GetTempRegForIntArg(uint32_t usedIntArgs,
                                       uint32_t usedFloatArgs, Register* out) {
  // NOTE: We can't properly determine which regs are used if there are
  // float arguments. If this is needed, we will have to guess.
  MOZ_ASSERT(usedFloatArgs == 0);

  if (GetIntArgReg(usedIntArgs, out)) {
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

}  // namespace jit
}  // namespace js
#endif /* jit_riscv64_Assembler_riscv64_h */
