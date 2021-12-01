/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "jit/arm/Simulator-arm.h"

#include "mozilla/Casting.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Likely.h"
#include "mozilla/MathAlgorithms.h"

#include "jit/arm/Assembler-arm.h"
#include "jit/arm/disasm/Constants-arm.h"
#include "jit/AtomicOperations.h"
#include "threading/LockGuard.h"
#include "vm/Runtime.h"
#include "vm/SharedMem.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmSignalHandlers.h"

extern "C" {

int64_t
__aeabi_idivmod(int x, int y)
{
    uint32_t lo = uint32_t(x / y);
    uint32_t hi = uint32_t(x % y);
    return (int64_t(hi) << 32) | lo;
}

int64_t
__aeabi_uidivmod(int x, int y)
{
    uint32_t lo = uint32_t(x) / uint32_t(y);
    uint32_t hi = uint32_t(x) % uint32_t(y);
    return (int64_t(hi) << 32) | lo;
}
}

namespace js {
namespace jit {

// For decoding load-exclusive and store-exclusive instructions.
namespace excl {

// Bit positions.
enum {
    ExclusiveOpHi = 24,         // Hi bit of opcode field
    ExclusiveOpLo = 23,         // Lo bit of opcode field
    ExclusiveSizeHi = 22,       // Hi bit of operand size field
    ExclusiveSizeLo = 21,       // Lo bit of operand size field
    ExclusiveLoad = 20          // Bit indicating load
};

// Opcode bits for exclusive instructions.
enum {
    ExclusiveOpcode = 3
};

// Operand size, Bits(ExclusiveSizeHi,ExclusiveSizeLo).
enum {
    ExclusiveWord = 0,
    ExclusiveDouble = 1,
    ExclusiveByte = 2,
    ExclusiveHalf = 3
};

}

// Load/store multiple addressing mode.
enum BlockAddrMode {
    // Alias modes for comparison when writeback does not matter.
    da_x         = (0|0|0) << 21,  // Decrement after.
    ia_x         = (0|4|0) << 21,  // Increment after.
    db_x         = (8|0|0) << 21,  // Decrement before.
    ib_x         = (8|4|0) << 21,  // Increment before.
};

// Type of VFP register. Determines register encoding.
enum VFPRegPrecision {
    kSinglePrecision = 0,
    kDoublePrecision = 1
};

enum NeonListType {
    nlt_1 = 0x7,
    nlt_2 = 0xA,
    nlt_3 = 0x6,
    nlt_4 = 0x2
};

// Supervisor Call (svc) specific support.

// Special Software Interrupt codes when used in the presence of the ARM
// simulator.
// svc (formerly swi) provides a 24bit immediate value. Use bits 22:0 for
// standard SoftwareInterrupCode. Bit 23 is reserved for the stop feature.
enum SoftwareInterruptCodes {
    kCallRtRedirected = 0x10,  // Transition to C code.
    kBreakpoint= 0x20, // Breakpoint.
    kStopCode = 1 << 23 // Stop.
};

const uint32_t kStopCodeMask = kStopCode - 1;
const uint32_t kMaxStopCode = kStopCode - 1;

// -----------------------------------------------------------------------------
// Instruction abstraction.

// The class Instruction enables access to individual fields defined in the ARM
// architecture instruction set encoding as described in figure A3-1.
// Note that the Assembler uses typedef int32_t Instr.
//
// Example: Test whether the instruction at ptr does set the condition code
// bits.
//
// bool InstructionSetsConditionCodes(byte* ptr) {
//   Instruction* instr = Instruction::At(ptr);
//   int type = instr->TypeValue();
//   return ((type == 0) || (type == 1)) && instr->hasS();
// }
//
class SimInstruction {
  public:
    enum {
        kInstrSize = 4,
        kPCReadOffset = 8
    };

    // Get the raw instruction bits.
    inline Instr instructionBits() const {
        return *reinterpret_cast<const Instr*>(this);
    }

    // Set the raw instruction bits to value.
    inline void setInstructionBits(Instr value) {
        *reinterpret_cast<Instr*>(this) = value;
    }

    // Read one particular bit out of the instruction bits.
    inline int bit(int nr) const {
        return (instructionBits() >> nr) & 1;
    }

    // Read a bit field's value out of the instruction bits.
    inline int bits(int hi, int lo) const {
        return (instructionBits() >> lo) & ((2 << (hi - lo)) - 1);
    }

    // Read a bit field out of the instruction bits.
    inline int bitField(int hi, int lo) const {
        return instructionBits() & (((2 << (hi - lo)) - 1) << lo);
    }

    // Accessors for the different named fields used in the ARM encoding.
    // The naming of these accessor corresponds to figure A3-1.
    //
    // Two kind of accessors are declared:
    // - <Name>Field() will return the raw field, i.e. the field's bits at their
    //   original place in the instruction encoding.
    //   e.g. if instr is the 'addgt r0, r1, r2' instruction, encoded as
    //   0xC0810002 conditionField(instr) will return 0xC0000000.
    // - <Name>Value() will return the field value, shifted back to bit 0.
    //   e.g. if instr is the 'addgt r0, r1, r2' instruction, encoded as
    //   0xC0810002 conditionField(instr) will return 0xC.

    // Generally applicable fields
    inline Assembler::ARMCondition conditionField() const {
        return static_cast<Assembler::ARMCondition>(bitField(31, 28));
    }
    inline int typeValue() const { return bits(27, 25); }
    inline int specialValue() const { return bits(27, 23); }

    inline int rnValue() const { return bits(19, 16); }
    inline int rdValue() const { return bits(15, 12); }

    inline int coprocessorValue() const { return bits(11, 8); }

    // Support for VFP.
    // Vn(19-16) | Vd(15-12) |  Vm(3-0)
    inline int vnValue() const { return bits(19, 16); }
    inline int vmValue() const { return bits(3, 0); }
    inline int vdValue() const { return bits(15, 12); }
    inline int nValue() const { return bit(7); }
    inline int mValue() const { return bit(5); }
    inline int dValue() const { return bit(22); }
    inline int rtValue() const { return bits(15, 12); }
    inline int pValue() const { return bit(24); }
    inline int uValue() const { return bit(23); }
    inline int opc1Value() const { return (bit(23) << 2) | bits(21, 20); }
    inline int opc2Value() const { return bits(19, 16); }
    inline int opc3Value() const { return bits(7, 6); }
    inline int szValue() const { return bit(8); }
    inline int VLValue() const { return bit(20); }
    inline int VCValue() const { return bit(8); }
    inline int VAValue() const { return bits(23, 21); }
    inline int VBValue() const { return bits(6, 5); }
    inline int VFPNRegValue(VFPRegPrecision pre) { return VFPGlueRegValue(pre, 16, 7); }
    inline int VFPMRegValue(VFPRegPrecision pre) { return VFPGlueRegValue(pre, 0, 5); }
    inline int VFPDRegValue(VFPRegPrecision pre) { return VFPGlueRegValue(pre, 12, 22); }

    // Fields used in Data processing instructions.
    inline int opcodeValue() const { return static_cast<ALUOp>(bits(24, 21)); }
    inline ALUOp opcodeField() const { return static_cast<ALUOp>(bitField(24, 21)); }
    inline int sValue() const { return bit(20); }

    // With register.
    inline int rmValue() const { return bits(3, 0); }
    inline ShiftType shifttypeValue() const { return static_cast<ShiftType>(bits(6, 5)); }
    inline int rsValue() const { return bits(11, 8); }
    inline int shiftAmountValue() const { return bits(11, 7); }

    // With immediate.
    inline int rotateValue() const { return bits(11, 8); }
    inline int immed8Value() const { return bits(7, 0); }
    inline int immed4Value() const { return bits(19, 16); }
    inline int immedMovwMovtValue() const { return immed4Value() << 12 | offset12Value(); }

    // Fields used in Load/Store instructions.
    inline int PUValue() const { return bits(24, 23); }
    inline int PUField() const { return bitField(24, 23); }
    inline int bValue() const { return bit(22); }
    inline int wValue() const { return bit(21); }
    inline int lValue() const { return bit(20); }

    // With register uses same fields as Data processing instructions above with
    // immediate.
    inline int offset12Value() const { return bits(11, 0); }

    // Multiple.
    inline int rlistValue() const { return bits(15, 0); }

    // Extra loads and stores.
    inline int signValue() const { return bit(6); }
    inline int hValue() const { return bit(5); }
    inline int immedHValue() const { return bits(11, 8); }
    inline int immedLValue() const { return bits(3, 0); }

    // Fields used in Branch instructions.
    inline int linkValue() const { return bit(24); }
    inline int sImmed24Value() const { return ((instructionBits() << 8) >> 8); }

    // Fields used in Software interrupt instructions.
    inline SoftwareInterruptCodes svcValue() const {
        return static_cast<SoftwareInterruptCodes>(bits(23, 0));
    }

    // Test for special encodings of type 0 instructions (extra loads and
    // stores, as well as multiplications).
    inline bool isSpecialType0() const { return (bit(7) == 1) && (bit(4) == 1); }

    // Test for miscellaneous instructions encodings of type 0 instructions.
    inline bool isMiscType0() const {
        return bit(24) == 1 && bit(23) == 0 && bit(20) == 0 && (bit(7) == 0);
    }

    // Test for a nop instruction, which falls under type 1.
    inline bool isNopType1() const { return bits(24, 0) == 0x0120F000; }

    // Test for a nop instruction, which falls under type 1.
    inline bool isCsdbType1() const { return bits(24, 0) == 0x0120F014; }

    // Test for a stop instruction.
    inline bool isStop() const {
        return typeValue() == 7 && bit(24) == 1 && svcValue() >= kStopCode;
    }

    // Test for a udf instruction, which falls under type 3.
    inline bool isUDF() const {
      return (instructionBits() & 0xfff000f0) == 0xe7f000f0;
    }

    // Special accessors that test for existence of a value.
    inline bool hasS()    const { return sValue() == 1; }
    inline bool hasB()    const { return bValue() == 1; }
    inline bool hasW()    const { return wValue() == 1; }
    inline bool hasL()    const { return lValue() == 1; }
    inline bool hasU()    const { return uValue() == 1; }
    inline bool hasSign() const { return signValue() == 1; }
    inline bool hasH()    const { return hValue() == 1; }
    inline bool hasLink() const { return linkValue() == 1; }

    // Decoding the double immediate in the vmov instruction.
    double doubleImmedVmov() const;
    // Decoding the float32 immediate in the vmov.f32 instruction.
    float float32ImmedVmov() const;

  private:
    // Join split register codes, depending on single or double precision.
    // four_bit is the position of the least-significant bit of the four
    // bit specifier. one_bit is the position of the additional single bit
    // specifier.
    inline int VFPGlueRegValue(VFPRegPrecision pre, int four_bit, int one_bit) {
        if (pre == kSinglePrecision)
            return (bits(four_bit + 3, four_bit) << 1) | bit(one_bit);
        return (bit(one_bit) << 4) | bits(four_bit + 3, four_bit);
    }

    SimInstruction() = delete;
    SimInstruction(const SimInstruction& other) = delete;
    void operator=(const SimInstruction& other) = delete;
};

double
SimInstruction::doubleImmedVmov() const
{
    // Reconstruct a double from the immediate encoded in the vmov instruction.
    //
    //   instruction: [xxxxxxxx,xxxxabcd,xxxxxxxx,xxxxefgh]
    //   double: [aBbbbbbb,bbcdefgh,00000000,00000000,
    //            00000000,00000000,00000000,00000000]
    //
    // where B = ~b. Only the high 16 bits are affected.
    uint64_t high16;
    high16  = (bits(17, 16) << 4) | bits(3, 0);   // xxxxxxxx,xxcdefgh.
    high16 |= (0xff * bit(18)) << 6;              // xxbbbbbb,bbxxxxxx.
    high16 |= (bit(18) ^ 1) << 14;                // xBxxxxxx,xxxxxxxx.
    high16 |= bit(19) << 15;                      // axxxxxxx,xxxxxxxx.

    uint64_t imm = high16 << 48;
    return mozilla::BitwiseCast<double>(imm);
}

float
SimInstruction::float32ImmedVmov() const
{
    // Reconstruct a float32 from the immediate encoded in the vmov instruction.
    //
    //   instruction: [xxxxxxxx,xxxxabcd,xxxxxxxx,xxxxefgh]
    //   float32: [aBbbbbbc, defgh000, 00000000, 00000000]
    //
    // where B = ~b. Only the high 16 bits are affected.
    uint32_t imm;
    imm  = (bits(17, 16) << 23) | (bits(3, 0) << 19); // xxxxxxxc,defgh000.0.0
    imm |= (0x1f * bit(18)) << 25;                    // xxbbbbbx,xxxxxxxx.0.0
    imm |= (bit(18) ^ 1) << 30;                       // xBxxxxxx,xxxxxxxx.0.0
    imm |= bit(19) << 31;                             // axxxxxxx,xxxxxxxx.0.0

    return mozilla::BitwiseCast<float>(imm);
}

class CachePage
{
  public:
    static const int LINE_VALID = 0;
    static const int LINE_INVALID = 1;
    static const int kPageShift = 12;
    static const int kPageSize = 1 << kPageShift;
    static const int kPageMask = kPageSize - 1;
    static const int kLineShift = 2;  // The cache line is only 4 bytes right now.
    static const int kLineLength = 1 << kLineShift;
    static const int kLineMask = kLineLength - 1;

    CachePage() {
        memset(&validity_map_, LINE_INVALID, sizeof(validity_map_));
    }
    char* validityByte(int offset) {
        return &validity_map_[offset >> kLineShift];
    }
    char* cachedData(int offset) {
        return &data_[offset];
    }

  private:
    char data_[kPageSize];   // The cached data.
    static const int kValidityMapSize = kPageSize >> kLineShift;
    char validity_map_[kValidityMapSize];  // One byte per line.
};

// Protects the icache() and redirection() properties of the
// Simulator.
class AutoLockSimulatorCache : public LockGuard<Mutex>
{
    using Base = LockGuard<Mutex>;

  public:
    explicit AutoLockSimulatorCache()
      : Base(SimulatorProcess::singleton_->cacheLock_)
    {}
};

mozilla::Atomic<size_t, mozilla::ReleaseAcquire>
    SimulatorProcess::ICacheCheckingDisableCount(1); // Checking is disabled by default.
mozilla::Atomic<bool, mozilla::ReleaseAcquire>
    SimulatorProcess::cacheInvalidatedBySignalHandler_(false);
SimulatorProcess* SimulatorProcess::singleton_ = nullptr;

int64_t Simulator::StopSimAt = -1L;

Simulator*
Simulator::Create(JSContext* cx)
{
    Simulator* sim = js_new<Simulator>(cx);
    if (!sim)
        return nullptr;

    if (!sim->init()) {
        js_delete(sim);
        return nullptr;
    }

    char* stopAtStr = getenv("ARM_SIM_STOP_AT");
    int64_t stopAt;
    if (stopAtStr && sscanf(stopAtStr, "%lld", &stopAt) == 1) {
        fprintf(stderr, "\nStopping simulation at icount %lld\n", stopAt);
        Simulator::StopSimAt = stopAt;
    }

    return sim;
}

void
Simulator::Destroy(Simulator* sim)
{
    js_delete(sim);
}

void
Simulator::disassemble(SimInstruction* instr, size_t n)
{
#ifdef JS_DISASM_ARM
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;
    while (n-- > 0) {
        dasm.InstructionDecode(buffer,
                               reinterpret_cast<uint8_t*>(instr));
        fprintf(stderr, "  0x%08x  %s\n", uint32_t(instr), buffer.start());
        instr = reinterpret_cast<SimInstruction*>(reinterpret_cast<uint8_t*>(instr) + 4);
    }
#endif
}

void
Simulator::disasm(SimInstruction* instr)
{
    disassemble(instr, 1);
}

void
Simulator::disasm(SimInstruction* instr, size_t n)
{
    disassemble(instr, n);
}

void
Simulator::disasm(SimInstruction* instr, size_t m, size_t n)
{
    disassemble(reinterpret_cast<SimInstruction*>(reinterpret_cast<uint8_t*>(instr)-m*4), n);
}

// The ArmDebugger class is used by the simulator while debugging simulated ARM
// code.
class ArmDebugger {
  public:
    explicit ArmDebugger(Simulator* sim) : sim_(sim) { }

    void stop(SimInstruction* instr);
    void debug();

  private:
    static const Instr kBreakpointInstr = (Assembler::AL | (7 * (1 << 25)) | (1* (1 << 24)) | kBreakpoint);
    static const Instr kNopInstr = (Assembler::AL | (13 * (1 << 21)));

    Simulator* sim_;

    int32_t getRegisterValue(int regnum);
    double getRegisterPairDoubleValue(int regnum);
    void getVFPDoubleRegisterValue(int regnum, double* value);
    bool getValue(const char* desc, int32_t* value);
    bool getVFPDoubleValue(const char* desc, double* value);

    // Set or delete a breakpoint. Returns true if successful.
    bool setBreakpoint(SimInstruction* breakpc);
    bool deleteBreakpoint(SimInstruction* breakpc);

    // Undo and redo all breakpoints. This is needed to bracket disassembly and
    // execution to skip past breakpoints when run from the debugger.
    void undoBreakpoints();
    void redoBreakpoints();
};

void
ArmDebugger::stop(SimInstruction * instr)
{
    // Get the stop code.
    uint32_t code = instr->svcValue() & kStopCodeMask;
    // Retrieve the encoded address, which comes just after this stop.
    char* msg = *reinterpret_cast<char**>(sim_->get_pc()
                                          + SimInstruction::kInstrSize);
    // Update this stop description.
    if (sim_->isWatchedStop(code) && !sim_->watched_stops_[code].desc) {
        sim_->watched_stops_[code].desc = msg;
    }
    // Print the stop message and code if it is not the default code.
    if (code != kMaxStopCode) {
        printf("Simulator hit stop %u: %s\n", code, msg);
    } else {
        printf("Simulator hit %s\n", msg);
    }
    sim_->set_pc(sim_->get_pc() + 2 * SimInstruction::kInstrSize);
    debug();
}

int32_t
ArmDebugger::getRegisterValue(int regnum)
{
    if (regnum == Registers::pc)
        return sim_->get_pc();
    return sim_->get_register(regnum);
}

double
ArmDebugger::getRegisterPairDoubleValue(int regnum)
{
    return sim_->get_double_from_register_pair(regnum);
}

void
ArmDebugger::getVFPDoubleRegisterValue(int regnum, double* out)
{
    sim_->get_double_from_d_register(regnum, out);
}

bool
ArmDebugger::getValue(const char* desc, int32_t* value)
{
    Register reg = Register::FromName(desc);
    if (reg != InvalidReg) {
        *value = getRegisterValue(reg.code());
        return true;
    }
    if (strncmp(desc, "0x", 2) == 0)
        return sscanf(desc + 2, "%x", reinterpret_cast<uint32_t*>(value)) == 1;
    return sscanf(desc, "%u", reinterpret_cast<uint32_t*>(value)) == 1;
}

bool
ArmDebugger::getVFPDoubleValue(const char* desc, double* value)
{
    FloatRegister reg = FloatRegister::FromCode(FloatRegister::FromName(desc));
    if (reg == InvalidFloatReg)
        return false;

    if (reg.isSingle()) {
        float fval;
        sim_->get_float_from_s_register(reg.id(), &fval);
        *value = fval;
        return true;
    }

    sim_->get_double_from_d_register(reg.id(), value);
    return true;
}

bool
ArmDebugger::setBreakpoint(SimInstruction* breakpc)
{
    // Check if a breakpoint can be set. If not return without any side-effects.
    if (sim_->break_pc_)
        return false;

    // Set the breakpoint.
    sim_->break_pc_ = breakpc;
    sim_->break_instr_ = breakpc->instructionBits();
    // Not setting the breakpoint instruction in the code itself. It will be set
    // when the debugger shell continues.
    return true;
}

bool
ArmDebugger::deleteBreakpoint(SimInstruction* breakpc)
{
    if (sim_->break_pc_ != nullptr)
        sim_->break_pc_->setInstructionBits(sim_->break_instr_);

    sim_->break_pc_ = nullptr;
    sim_->break_instr_ = 0;
    return true;
}

void
ArmDebugger::undoBreakpoints()
{
    if (sim_->break_pc_)
        sim_->break_pc_->setInstructionBits(sim_->break_instr_);
}

void
ArmDebugger::redoBreakpoints()
{
    if (sim_->break_pc_)
        sim_->break_pc_->setInstructionBits(kBreakpointInstr);
}

static char*
ReadLine(const char* prompt)
{
    char* result = nullptr;
    char line_buf[256];
    int offset = 0;
    bool keep_going = true;
    fprintf(stdout, "%s", prompt);
    fflush(stdout);
    while (keep_going) {
        if (fgets(line_buf, sizeof(line_buf), stdin) == nullptr) {
            // fgets got an error. Just give up.
            if (result)
                js_delete(result);
            return nullptr;
        }
        int len = strlen(line_buf);
        if (len > 0 && line_buf[len - 1] == '\n') {
            // Since we read a new line we are done reading the line. This will
            // exit the loop after copying this buffer into the result.
            keep_going = false;
        }
        if (!result) {
            // Allocate the initial result and make room for the terminating
            // '\0'.
            result = (char*)js_malloc(len + 1);
            if (!result)
                return nullptr;
        } else {
            // Allocate a new result with enough room for the new addition.
            int new_len = offset + len + 1;
            char* new_result = (char*)js_malloc(new_len);
            if (!new_result)
                return nullptr;
            // Copy the existing input into the new array and set the new
            // array as the result.
            memcpy(new_result, result, offset * sizeof(char));
            js_free(result);
            result = new_result;
        }
        // Copy the newly read line into the result.
        memcpy(result + offset, line_buf, len * sizeof(char));
        offset += len;
    }

    MOZ_ASSERT(result);
    result[offset] = '\0';
    return result;
}


void
ArmDebugger::debug()
{
    intptr_t last_pc = -1;
    bool done = false;

#define COMMAND_SIZE 63
#define ARG_SIZE 255

#define STR(a) #a
#define XSTR(a) STR(a)

    char cmd[COMMAND_SIZE + 1];
    char arg1[ARG_SIZE + 1];
    char arg2[ARG_SIZE + 1];
    char* argv[3] = { cmd, arg1, arg2 };

    // Make sure to have a proper terminating character if reaching the limit.
    cmd[COMMAND_SIZE] = 0;
    arg1[ARG_SIZE] = 0;
    arg2[ARG_SIZE] = 0;

    // Undo all set breakpoints while running in the debugger shell. This will
    // make them invisible to all commands.
    undoBreakpoints();

#ifndef JS_DISASM_ARM
    static bool disasm_warning_printed = false;
    if (!disasm_warning_printed) {
        printf("  No ARM disassembler present.  Enable JS_DISASM_ARM in configure.in.");
        disasm_warning_printed = true;
    }
#endif

    while (!done && !sim_->has_bad_pc()) {
        if (last_pc != sim_->get_pc()) {
#ifdef JS_DISASM_ARM
            disasm::NameConverter converter;
            disasm::Disassembler dasm(converter);
            disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;
            dasm.InstructionDecode(buffer,
                                   reinterpret_cast<uint8_t*>(sim_->get_pc()));
            printf("  0x%08x  %s\n", sim_->get_pc(), buffer.start());
#endif
            last_pc = sim_->get_pc();
        }
        char* line = ReadLine("sim> ");
        if (line == nullptr) {
            break;
        } else {
            char* last_input = sim_->lastDebuggerInput();
            if (strcmp(line, "\n") == 0 && last_input != nullptr) {
                line = last_input;
            } else {
                // Ownership is transferred to sim_;
                sim_->setLastDebuggerInput(line);
            }

            // Use sscanf to parse the individual parts of the command line. At the
            // moment no command expects more than two parameters.
            int argc = sscanf(line,
                              "%" XSTR(COMMAND_SIZE) "s "
                              "%" XSTR(ARG_SIZE) "s "
                              "%" XSTR(ARG_SIZE) "s",
                              cmd, arg1, arg2);
            if (argc < 0) {
                continue;
            } else if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
                sim_->instructionDecode(reinterpret_cast<SimInstruction*>(sim_->get_pc()));
                sim_->icount_++;
            } else if ((strcmp(cmd, "skip") == 0)) {
                sim_->set_pc(sim_->get_pc() + 4);
                sim_->icount_++;
            } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
                // Execute the one instruction we broke at with breakpoints
                // disabled.
                sim_->instructionDecode(reinterpret_cast<SimInstruction*>(sim_->get_pc()));
                sim_->icount_++;
                // Leave the debugger shell.
                done = true;
            } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
                if (argc == 2 || (argc == 3 && strcmp(arg2, "fp") == 0)) {
                    int32_t value;
                    double dvalue;
                    if (strcmp(arg1, "all") == 0) {
                        for (uint32_t i = 0; i < Registers::Total; i++) {
                            value = getRegisterValue(i);
                            printf("%3s: 0x%08x %10d", Registers::GetName(i), value, value);
                            if ((argc == 3 && strcmp(arg2, "fp") == 0) &&
                                i < 8 &&
                                (i % 2) == 0) {
                                dvalue = getRegisterPairDoubleValue(i);
                                printf(" (%.16g)\n", dvalue);
                            } else {
                                printf("\n");
                            }
                        }
                        for (uint32_t i = 0; i < FloatRegisters::TotalPhys; i++) {
                            getVFPDoubleRegisterValue(i, &dvalue);
                            uint64_t as_words = mozilla::BitwiseCast<uint64_t>(dvalue);
                            printf("%3s: %.16g 0x%08x %08x\n",
                                   FloatRegister::FromCode(i).name(),
                                   dvalue,
                                   static_cast<uint32_t>(as_words >> 32),
                                   static_cast<uint32_t>(as_words & 0xffffffff));
                        }
                    } else {
                        if (getValue(arg1, &value)) {
                            printf("%s: 0x%08x %d \n", arg1, value, value);
                        } else if (getVFPDoubleValue(arg1, &dvalue)) {
                            uint64_t as_words = mozilla::BitwiseCast<uint64_t>(dvalue);
                            printf("%s: %.16g 0x%08x %08x\n",
                                   arg1,
                                   dvalue,
                                   static_cast<uint32_t>(as_words >> 32),
                                   static_cast<uint32_t>(as_words & 0xffffffff));
                        } else {
                            printf("%s unrecognized\n", arg1);
                        }
                    }
                } else {
                    printf("print <register>\n");
                }
            } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
                int32_t* cur = nullptr;
                int32_t* end = nullptr;
                int next_arg = 1;

                if (strcmp(cmd, "stack") == 0) {
                    cur = reinterpret_cast<int32_t*>(sim_->get_register(Simulator::sp));
                } else {  // "mem"
                    int32_t value;
                    if (!getValue(arg1, &value)) {
                        printf("%s unrecognized\n", arg1);
                        continue;
                    }
                    cur = reinterpret_cast<int32_t*>(value);
                    next_arg++;
                }

                int32_t words;
                if (argc == next_arg) {
                    words = 10;
                } else {
                    if (!getValue(argv[next_arg], &words)) {
                        words = 10;
                    }
                }
                end = cur + words;

                while (cur < end) {
                    printf("  %p:  0x%08x %10d", cur, *cur, *cur);
                    printf("\n");
                    cur++;
                }
            } else if (strcmp(cmd, "disasm") == 0 || strcmp(cmd, "di") == 0) {
#ifdef JS_DISASM_ARM
                uint8_t* prev = nullptr;
                uint8_t* cur = nullptr;
                uint8_t* end = nullptr;

                if (argc == 1) {
                    cur = reinterpret_cast<uint8_t*>(sim_->get_pc());
                    end = cur + (10 * SimInstruction::kInstrSize);
                } else if (argc == 2) {
                    Register reg = Register::FromName(arg1);
                    if (reg != InvalidReg || strncmp(arg1, "0x", 2) == 0) {
                        // The argument is an address or a register name.
                        int32_t value;
                        if (getValue(arg1, &value)) {
                            cur = reinterpret_cast<uint8_t*>(value);
                            // Disassemble 10 instructions at <arg1>.
                            end = cur + (10 * SimInstruction::kInstrSize);
                        }
                    } else {
                        // The argument is the number of instructions.
                        int32_t value;
                        if (getValue(arg1, &value)) {
                            cur = reinterpret_cast<uint8_t*>(sim_->get_pc());
                            // Disassemble <arg1> instructions.
                            end = cur + (value * SimInstruction::kInstrSize);
                        }
                    }
                } else {
                    int32_t value1;
                    int32_t value2;
                    if (getValue(arg1, &value1) && getValue(arg2, &value2)) {
                        cur = reinterpret_cast<uint8_t*>(value1);
                        end = cur + (value2 * SimInstruction::kInstrSize);
                    }
                }
                while (cur < end) {
                    disasm::NameConverter converter;
                    disasm::Disassembler dasm(converter);
                    disasm::EmbeddedVector<char, disasm::ReasonableBufferSize> buffer;

                    prev = cur;
                    cur += dasm.InstructionDecode(buffer, cur);
                    printf("  0x%08x  %s\n", reinterpret_cast<uint32_t>(prev), buffer.start());
                }
#endif
            } else if (strcmp(cmd, "gdb") == 0) {
                printf("relinquishing control to gdb\n");
                asm("int $3");
                printf("regaining control from gdb\n");
            } else if (strcmp(cmd, "break") == 0) {
                if (argc == 2) {
                    int32_t value;
                    if (getValue(arg1, &value)) {
                        if (!setBreakpoint(reinterpret_cast<SimInstruction*>(value)))
                            printf("setting breakpoint failed\n");
                    } else {
                        printf("%s unrecognized\n", arg1);
                    }
                } else {
                    printf("break <address>\n");
                }
            } else if (strcmp(cmd, "del") == 0) {
                if (!deleteBreakpoint(nullptr)) {
                    printf("deleting breakpoint failed\n");
                }
            } else if (strcmp(cmd, "flags") == 0) {
                printf("N flag: %d; ", sim_->n_flag_);
                printf("Z flag: %d; ", sim_->z_flag_);
                printf("C flag: %d; ", sim_->c_flag_);
                printf("V flag: %d\n", sim_->v_flag_);
                printf("INVALID OP flag: %d; ", sim_->inv_op_vfp_flag_);
                printf("DIV BY ZERO flag: %d; ", sim_->div_zero_vfp_flag_);
                printf("OVERFLOW flag: %d; ", sim_->overflow_vfp_flag_);
                printf("UNDERFLOW flag: %d; ", sim_->underflow_vfp_flag_);
                printf("INEXACT flag: %d;\n", sim_->inexact_vfp_flag_);
            } else if (strcmp(cmd, "stop") == 0) {
                int32_t value;
                intptr_t stop_pc = sim_->get_pc() - 2 * SimInstruction::kInstrSize;
                SimInstruction* stop_instr = reinterpret_cast<SimInstruction*>(stop_pc);
                SimInstruction* msg_address =
                    reinterpret_cast<SimInstruction*>(stop_pc + SimInstruction::kInstrSize);
                if ((argc == 2) && (strcmp(arg1, "unstop") == 0)) {
                    // Remove the current stop.
                    if (sim_->isStopInstruction(stop_instr)) {
                        stop_instr->setInstructionBits(kNopInstr);
                        msg_address->setInstructionBits(kNopInstr);
                    } else {
                        printf("Not at debugger stop.\n");
                    }
                } else if (argc == 3) {
                    // Print information about all/the specified breakpoint(s).
                    if (strcmp(arg1, "info") == 0) {
                        if (strcmp(arg2, "all") == 0) {
                            printf("Stop information:\n");
                            for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++)
                                sim_->printStopInfo(i);
                        } else if (getValue(arg2, &value)) {
                            sim_->printStopInfo(value);
                        } else {
                            printf("Unrecognized argument.\n");
                        }
                    } else if (strcmp(arg1, "enable") == 0) {
                        // Enable all/the specified breakpoint(s).
                        if (strcmp(arg2, "all") == 0) {
                            for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++)
                                sim_->enableStop(i);
                        } else if (getValue(arg2, &value)) {
                            sim_->enableStop(value);
                        } else {
                            printf("Unrecognized argument.\n");
                        }
                    } else if (strcmp(arg1, "disable") == 0) {
                        // Disable all/the specified breakpoint(s).
                        if (strcmp(arg2, "all") == 0) {
                            for (uint32_t i = 0; i < sim_->kNumOfWatchedStops; i++) {
                                sim_->disableStop(i);
                            }
                        } else if (getValue(arg2, &value)) {
                            sim_->disableStop(value);
                        } else {
                            printf("Unrecognized argument.\n");
                        }
                    }
                } else {
                    printf("Wrong usage. Use help command for more information.\n");
                }
            } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
                printf("cont\n");
                printf("  continue execution (alias 'c')\n");
                printf("skip\n");
                printf("  skip one instruction (set pc to next instruction)\n");
                printf("stepi\n");
                printf("  step one instruction (alias 'si')\n");
                printf("print <register>\n");
                printf("  print register content (alias 'p')\n");
                printf("  use register name 'all' to print all registers\n");
                printf("  add argument 'fp' to print register pair double values\n");
                printf("flags\n");
                printf("  print flags\n");
                printf("stack [<words>]\n");
                printf("  dump stack content, default dump 10 words)\n");
                printf("mem <address> [<words>]\n");
                printf("  dump memory content, default dump 10 words)\n");
                printf("disasm [<instructions>]\n");
                printf("disasm [<address/register>]\n");
                printf("disasm [[<address/register>] <instructions>]\n");
                printf("  disassemble code, default is 10 instructions\n");
                printf("  from pc (alias 'di')\n");
                printf("gdb\n");
                printf("  enter gdb\n");
                printf("break <address>\n");
                printf("  set a break point on the address\n");
                printf("del\n");
                printf("  delete the breakpoint\n");
                printf("stop feature:\n");
                printf("  Description:\n");
                printf("    Stops are debug instructions inserted by\n");
                printf("    the Assembler::stop() function.\n");
                printf("    When hitting a stop, the Simulator will\n");
                printf("    stop and and give control to the ArmDebugger.\n");
                printf("    The first %d stop codes are watched:\n",
                       Simulator::kNumOfWatchedStops);
                printf("    - They can be enabled / disabled: the Simulator\n");
                printf("      will / won't stop when hitting them.\n");
                printf("    - The Simulator keeps track of how many times they \n");
                printf("      are met. (See the info command.) Going over a\n");
                printf("      disabled stop still increases its counter. \n");
                printf("  Commands:\n");
                printf("    stop info all/<code> : print infos about number <code>\n");
                printf("      or all stop(s).\n");
                printf("    stop enable/disable all/<code> : enables / disables\n");
                printf("      all or number <code> stop(s)\n");
                printf("    stop unstop\n");
                printf("      ignore the stop instruction at the current location\n");
                printf("      from now on\n");
            } else {
                printf("Unknown command: %s\n", cmd);
            }
        }
    }

    // Add all the breakpoints back to stop execution and enter the debugger
    // shell when hit.
    redoBreakpoints();

#undef COMMAND_SIZE
#undef ARG_SIZE

#undef STR
#undef XSTR
}

static bool
AllOnOnePage(uintptr_t start, int size)
{
    intptr_t start_page = (start & ~CachePage::kPageMask);
    intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
    return start_page == end_page;
}

static CachePage*
GetCachePageLocked(SimulatorProcess::ICacheMap& i_cache, void* page)
{
    SimulatorProcess::ICacheMap::AddPtr p = i_cache.lookupForAdd(page);
    if (p)
        return p->value();

    AutoEnterOOMUnsafeRegion oomUnsafe;
    CachePage* new_page = js_new<CachePage>();
    if (!new_page || !i_cache.add(p, page, new_page))
        oomUnsafe.crash("Simulator CachePage");

    return new_page;
}

// Flush from start up to and not including start + size.
static void
FlushOnePageLocked(SimulatorProcess::ICacheMap& i_cache, intptr_t start, int size)
{
    MOZ_ASSERT(size <= CachePage::kPageSize);
    MOZ_ASSERT(AllOnOnePage(start, size - 1));
    MOZ_ASSERT((start & CachePage::kLineMask) == 0);
    MOZ_ASSERT((size & CachePage::kLineMask) == 0);

    void* page = reinterpret_cast<void*>(start & (~CachePage::kPageMask));
    int offset = (start & CachePage::kPageMask);
    CachePage* cache_page = GetCachePageLocked(i_cache, page);
    char* valid_bytemap = cache_page->validityByte(offset);
    memset(valid_bytemap, CachePage::LINE_INVALID, size >> CachePage::kLineShift);
}

static void
FlushICacheLocked(SimulatorProcess::ICacheMap& i_cache, void* start_addr, size_t size)
{
    intptr_t start = reinterpret_cast<intptr_t>(start_addr);
    int intra_line = (start & CachePage::kLineMask);
    start -= intra_line;
    size += intra_line;
    size = ((size - 1) | CachePage::kLineMask) + 1;
    int offset = (start & CachePage::kPageMask);
    while (!AllOnOnePage(start, size - 1)) {
        int bytes_to_flush = CachePage::kPageSize - offset;
        FlushOnePageLocked(i_cache, start, bytes_to_flush);
        start += bytes_to_flush;
        size -= bytes_to_flush;
        MOZ_ASSERT((start & CachePage::kPageMask) == 0);
        offset = 0;
    }
    if (size != 0)
        FlushOnePageLocked(i_cache, start, size);
}

/* static */ void
SimulatorProcess::checkICacheLocked(SimInstruction* instr)
{
    intptr_t address = reinterpret_cast<intptr_t>(instr);
    void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
    void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
    int offset = (address & CachePage::kPageMask);
    CachePage* cache_page = GetCachePageLocked(icache(), page);
    char* cache_valid_byte = cache_page->validityByte(offset);
    bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
    char* cached_line = cache_page->cachedData(offset & ~CachePage::kLineMask);

    // Read all state before considering signal handler effects.
    int cmpret = 0;
    if (cache_hit) {
        // Check that the data in memory matches the contents of the I-cache.
        cmpret = memcmp(reinterpret_cast<void*>(instr),
                        cache_page->cachedData(offset),
                        SimInstruction::kInstrSize);
    }

    // Check for signal handler interruption between reading state and asserting.
    // It is safe for the signal to arrive during the !cache_hit path, since it
    // will be cleared the next time this function is called.
    if (cacheInvalidatedBySignalHandler_) {
        icache().clear();
        cacheInvalidatedBySignalHandler_ = false;
        return;
    }

    if (cache_hit) {
        MOZ_ASSERT(cmpret == 0);
    } else {
        // Cache miss. Load memory into the cache.
        memcpy(cached_line, line, CachePage::kLineLength);
        *cache_valid_byte = CachePage::LINE_VALID;
    }
}

HashNumber
SimulatorProcess::ICacheHasher::hash(const Lookup& l)
{
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(l)) >> 2;
}

bool
SimulatorProcess::ICacheHasher::match(const Key& k, const Lookup& l)
{
    MOZ_ASSERT((reinterpret_cast<intptr_t>(k) & CachePage::kPageMask) == 0);
    MOZ_ASSERT((reinterpret_cast<intptr_t>(l) & CachePage::kPageMask) == 0);
    return k == l;
}

void
Simulator::setLastDebuggerInput(char* input)
{
    js_free(lastDebuggerInput_);
    lastDebuggerInput_ = input;
}

/* static */ void
SimulatorProcess::FlushICache(void* start_addr, size_t size)
{
    JitSpewCont(JitSpew_CacheFlush, "[%p %zx]", start_addr, size);
    if (!ICacheCheckingDisableCount) {
        AutoLockSimulatorCache als;
        js::jit::FlushICacheLocked(icache(), start_addr, size);
    }
}

Simulator::Simulator(JSContext* cx)
  : cx_(cx)
{
    // Set up simulator support first. Some of this information is needed to
    // setup the architecture state.

    // Note, allocation and anything that depends on allocated memory is
    // deferred until init(), in order to handle OOM properly.

    stack_ = nullptr;
    stackLimit_ = 0;
    pc_modified_ = false;
    icount_ = 0L;
    wasm_interrupt_ = false;
    break_pc_ = nullptr;
    break_instr_ = 0;
    single_stepping_ = false;
    single_step_callback_ = nullptr;
    single_step_callback_arg_ = nullptr;
    skipCalleeSavedRegsCheck = false;

    // Set up architecture state.
    // All registers are initialized to zero to start with.
    for (int i = 0; i < num_registers; i++)
        registers_[i] = 0;

    n_flag_ = false;
    z_flag_ = false;
    c_flag_ = false;
    v_flag_ = false;

    for (int i = 0; i < num_d_registers * 2; i++)
        vfp_registers_[i] = 0;

    n_flag_FPSCR_ = false;
    z_flag_FPSCR_ = false;
    c_flag_FPSCR_ = false;
    v_flag_FPSCR_ = false;
    FPSCR_rounding_mode_ = SimRZ;
    FPSCR_default_NaN_mode_ = true;

    inv_op_vfp_flag_ = false;
    div_zero_vfp_flag_ = false;
    overflow_vfp_flag_ = false;
    underflow_vfp_flag_ = false;
    inexact_vfp_flag_ = false;

    // The lr and pc are initialized to a known bad value that will cause an
    // access violation if the simulator ever tries to execute it.
    registers_[pc] = bad_lr;
    registers_[lr] = bad_lr;

    lastDebuggerInput_ = nullptr;

    exclusiveMonitorHeld_ = false;
    exclusiveMonitor_ = 0;
}

bool
Simulator::init()
{
    // Allocate 2MB for the stack. Note that we will only use 1MB, see below.
    static const size_t stackSize = 2 * 1024*1024;
    stack_ = reinterpret_cast<char*>(js_malloc(stackSize));
    if (!stack_)
        return false;

    // Leave a safety margin of 1MB to prevent overrunning the stack when
    // pushing values (total stack size is 2MB).
    stackLimit_ = reinterpret_cast<uintptr_t>(stack_) + 1024 * 1024;

    // The sp is initialized to point to the bottom (high address) of the
    // allocated stack area. To be safe in potential stack underflows we leave
    // some buffer below.
    registers_[sp] = reinterpret_cast<int32_t>(stack_) + stackSize - 64;

    return true;
}

// When the generated code calls a VM function (masm.callWithABI) we need to
// call that function instead of trying to execute it with the simulator
// (because it's x86 code instead of arm code). We do that by redirecting the VM
// call to a svc (Supervisor Call) instruction that is handled by the
// simulator. We write the original destination of the jump just at a known
// offset from the svc instruction so the simulator knows what to call.
class Redirection
{
    friend class SimulatorProcess;

    // sim's lock must already be held.
    Redirection(void* nativeFunction, ABIFunctionType type)
      : nativeFunction_(nativeFunction),
        swiInstruction_(Assembler::AL | (0xf * (1 << 24)) | kCallRtRedirected),
        type_(type),
        next_(nullptr)
    {
        next_ = SimulatorProcess::redirection();
        if (!SimulatorProcess::ICacheCheckingDisableCount) {
            FlushICacheLocked(SimulatorProcess::icache(), addressOfSwiInstruction(),
                              SimInstruction::kInstrSize);
        }
        SimulatorProcess::setRedirection(this);
    }

  public:
    void* addressOfSwiInstruction() { return &swiInstruction_; }
    void* nativeFunction() const { return nativeFunction_; }
    ABIFunctionType type() const { return type_; }

    static Redirection* Get(void* nativeFunction, ABIFunctionType type) {
        AutoLockSimulatorCache als;

        Redirection* current = SimulatorProcess::redirection();
        for (; current != nullptr; current = current->next_) {
            if (current->nativeFunction_ == nativeFunction) {
                MOZ_ASSERT(current->type() == type);
                return current;
            }
        }

        AutoEnterOOMUnsafeRegion oomUnsafe;
        Redirection* redir = (Redirection*)js_malloc(sizeof(Redirection));
        if (!redir)
            oomUnsafe.crash("Simulator redirection");
        new(redir) Redirection(nativeFunction, type);
        return redir;
    }

    static Redirection* FromSwiInstruction(SimInstruction* swiInstruction) {
        uint8_t* addrOfSwi = reinterpret_cast<uint8_t*>(swiInstruction);
        uint8_t* addrOfRedirection = addrOfSwi - offsetof(Redirection, swiInstruction_);
        return reinterpret_cast<Redirection*>(addrOfRedirection);
    }

  private:
    void* nativeFunction_;
    uint32_t swiInstruction_;
    ABIFunctionType type_;
    Redirection* next_;
};

Simulator::~Simulator()
{
    js_free(stack_);
}

SimulatorProcess::SimulatorProcess()
  : cacheLock_(mutexid::SimulatorCacheLock)
  , redirection_(nullptr)
{}

SimulatorProcess::~SimulatorProcess()
{
    Redirection* r = redirection_;
    while (r) {
        Redirection* next = r->next_;
        js_delete(r);
        r = next;
    }
}

bool
SimulatorProcess::init()
{
    if (getenv("ARM_SIM_ICACHE_CHECKS"))
        ICacheCheckingDisableCount = 0;

    return icache_.init();
}

/* static */ void*
Simulator::RedirectNativeFunction(void* nativeFunction, ABIFunctionType type)
{
    Redirection* redirection = Redirection::Get(nativeFunction, type);
    return redirection->addressOfSwiInstruction();
}

// Sets the register in the architecture state. It will also deal with updating
// Simulator internal state for special registers such as PC.
void
Simulator::set_register(int reg, int32_t value)
{
    MOZ_ASSERT(reg >= 0 && reg < num_registers);
    if (reg == pc)
        pc_modified_ = true;
    registers_[reg] = value;
}

// Get the register from the architecture state. This function does handle the
// special case of accessing the PC register.
int32_t
Simulator::get_register(int reg) const
{
    MOZ_ASSERT(reg >= 0 && reg < num_registers);
    // Work around GCC bug: http://gcc.gnu.org/bugzilla/show_bug.cgi?id=43949
    if (reg >= num_registers) return 0;
    return registers_[reg] + ((reg == pc) ? SimInstruction::kPCReadOffset : 0);
}

double
Simulator::get_double_from_register_pair(int reg)
{
    MOZ_ASSERT(reg >= 0 && reg < num_registers && (reg % 2) == 0);

    // Read the bits from the unsigned integer register_[] array into the double
    // precision floating point value and return it.
    double dm_val = 0.0;
    char buffer[2 * sizeof(vfp_registers_[0])];
    memcpy(buffer, &registers_[reg], 2 * sizeof(registers_[0]));
    memcpy(&dm_val, buffer, 2 * sizeof(registers_[0]));
    return dm_val;
}

void
Simulator::set_register_pair_from_double(int reg, double* value)
{
    MOZ_ASSERT(reg >= 0 && reg < num_registers && (reg % 2) == 0);
    memcpy(registers_ + reg, value, sizeof(*value));
}

void
Simulator::set_dw_register(int dreg, const int* dbl)
{
    MOZ_ASSERT(dreg >= 0 && dreg < num_d_registers);
    registers_[dreg] = dbl[0];
    registers_[dreg + 1] = dbl[1];
}

void
Simulator::get_d_register(int dreg, uint64_t* value)
{
    MOZ_ASSERT(dreg >= 0 && dreg < int(FloatRegisters::TotalPhys));
    memcpy(value, vfp_registers_ + dreg * 2, sizeof(*value));
}

void
Simulator::set_d_register(int dreg, const uint64_t* value)
{
    MOZ_ASSERT(dreg >= 0 && dreg < int(FloatRegisters::TotalPhys));
    memcpy(vfp_registers_ + dreg * 2, value, sizeof(*value));
}

void
Simulator::get_d_register(int dreg, uint32_t* value)
{
    MOZ_ASSERT(dreg >= 0 && dreg < int(FloatRegisters::TotalPhys));
    memcpy(value, vfp_registers_ + dreg * 2, sizeof(*value) * 2);
}

void
Simulator::set_d_register(int dreg, const uint32_t* value)
{
    MOZ_ASSERT(dreg >= 0 && dreg < int(FloatRegisters::TotalPhys));
    memcpy(vfp_registers_ + dreg * 2, value, sizeof(*value) * 2);
}

void
Simulator::get_q_register(int qreg, uint64_t* value)
{
    MOZ_ASSERT(qreg >= 0 && qreg < num_q_registers);
    memcpy(value, vfp_registers_ + qreg * 4, sizeof(*value) * 2);
}

void
Simulator::set_q_register(int qreg, const uint64_t* value)
{
    MOZ_ASSERT(qreg >= 0 && qreg < num_q_registers);
    memcpy(vfp_registers_ + qreg * 4, value, sizeof(*value) * 2);
}

void
Simulator::get_q_register(int qreg, uint32_t* value)
{
    MOZ_ASSERT(qreg >= 0 && qreg < num_q_registers);
    memcpy(value, vfp_registers_ + qreg * 4, sizeof(*value) * 4);
}

void
Simulator::set_q_register(int qreg, const uint32_t* value)
{
    MOZ_ASSERT((qreg >= 0) && (qreg < num_q_registers));
    memcpy(vfp_registers_ + qreg * 4, value, sizeof(*value) * 4);
}

void
Simulator::set_pc(int32_t value)
{
    pc_modified_ = true;
    registers_[pc] = value;
}

bool
Simulator::has_bad_pc() const
{
    return registers_[pc] == bad_lr || registers_[pc] == end_sim_pc;
}

// Raw access to the PC register without the special adjustment when reading.
int32_t
Simulator::get_pc() const
{
    return registers_[pc];
}

void
Simulator::set_s_register(int sreg, unsigned int value)
{
    MOZ_ASSERT(sreg >= 0 && sreg < num_s_registers);
    vfp_registers_[sreg] = value;
}

unsigned
Simulator::get_s_register(int sreg) const
{
    MOZ_ASSERT(sreg >= 0 && sreg < num_s_registers);
    return vfp_registers_[sreg];
}

template<class InputType, int register_size>
void
Simulator::setVFPRegister(int reg_index, const InputType& value)
{
    MOZ_ASSERT(reg_index >= 0);
    MOZ_ASSERT_IF(register_size == 1, reg_index < num_s_registers);
    MOZ_ASSERT_IF(register_size == 2, reg_index < int(FloatRegisters::TotalPhys));

    char buffer[register_size * sizeof(vfp_registers_[0])];
    memcpy(buffer, &value, register_size * sizeof(vfp_registers_[0]));
    memcpy(&vfp_registers_[reg_index * register_size], buffer,
           register_size * sizeof(vfp_registers_[0]));
}

template<class ReturnType, int register_size>
void Simulator::getFromVFPRegister(int reg_index, ReturnType* out)
{
    MOZ_ASSERT(reg_index >= 0);
    MOZ_ASSERT_IF(register_size == 1, reg_index < num_s_registers);
    MOZ_ASSERT_IF(register_size == 2, reg_index < int(FloatRegisters::TotalPhys));

    char buffer[register_size * sizeof(vfp_registers_[0])];
    memcpy(buffer, &vfp_registers_[register_size * reg_index],
           register_size * sizeof(vfp_registers_[0]));
    memcpy(out, buffer, register_size * sizeof(vfp_registers_[0]));
}

// These forced-instantiations are for jsapi-tests. Evidently, nothing
// requires these to be instantiated.
template void Simulator::getFromVFPRegister<double, 2>(int reg_index, double* out);
template void Simulator::getFromVFPRegister<float, 1>(int reg_index, float* out);
template void Simulator::setVFPRegister<double, 2>(int reg_index, const double& value);
template void Simulator::setVFPRegister<float, 1>(int reg_index, const float& value);

void
Simulator::getFpArgs(double* x, double* y, int32_t* z)
{
    if (UseHardFpABI()) {
        get_double_from_d_register(0, x);
        get_double_from_d_register(1, y);
        *z = get_register(0);
    } else {
        *x = get_double_from_register_pair(0);
        *y = get_double_from_register_pair(2);
        *z = get_register(2);
    }
}

void
Simulator::getFpFromStack(int32_t* stack, double* x)
{
    MOZ_ASSERT(stack && x);
    char buffer[2 * sizeof(stack[0])];
    memcpy(buffer, stack, 2 * sizeof(stack[0]));
    memcpy(x, buffer, 2 * sizeof(stack[0]));
}

void
Simulator::setCallResultDouble(double result)
{
    // The return value is either in r0/r1 or d0.
    if (UseHardFpABI()) {
        char buffer[2 * sizeof(vfp_registers_[0])];
        memcpy(buffer, &result, sizeof(buffer));
        // Copy result to d0.
        memcpy(vfp_registers_, buffer, sizeof(buffer));
    } else {
        char buffer[2 * sizeof(registers_[0])];
        memcpy(buffer, &result, sizeof(buffer));
        // Copy result to r0 and r1.
        memcpy(registers_, buffer, sizeof(buffer));
    }
}

void
Simulator::setCallResultFloat(float result)
{
    if (UseHardFpABI()) {
        char buffer[sizeof(registers_[0])];
        memcpy(buffer, &result, sizeof(buffer));
        // Copy result to s0.
        memcpy(vfp_registers_, buffer, sizeof(buffer));
    } else {
        char buffer[sizeof(registers_[0])];
        memcpy(buffer, &result, sizeof(buffer));
        // Copy result to r0.
        memcpy(registers_, buffer, sizeof(buffer));
    }
}

void
Simulator::setCallResult(int64_t res)
{
    set_register(r0, static_cast<int32_t>(res));
    set_register(r1, static_cast<int32_t>(res >> 32));
}

void
Simulator::exclusiveMonitorSet(uint64_t value)
{
    exclusiveMonitor_ = value;
    exclusiveMonitorHeld_ = true;
}

uint64_t
Simulator::exclusiveMonitorGetAndClear(bool* held)
{
    *held = exclusiveMonitorHeld_;
    exclusiveMonitorHeld_ = false;
    return *held ? exclusiveMonitor_ : 0;
}

void
Simulator::exclusiveMonitorClear()
{
    exclusiveMonitorHeld_ = false;
}

JS::ProfilingFrameIterator::RegisterState
Simulator::registerState()
{
    wasm::RegisterState state;
    state.pc = (void*) get_pc();
    state.fp = (void*) get_register(fp);
    state.sp = (void*) get_register(sp);
    state.lr = (void*) get_register(lr);
    return state;
}

// The signal handler only redirects the PC to the interrupt stub when the PC is
// in function code. However, this guard is racy for the ARM simulator since the
// signal handler samples PC in the middle of simulating an instruction and thus
// the current PC may have advanced once since the signal handler's guard. So we
// re-check here.
void
Simulator::handleWasmInterrupt()
{
    if (!wasm::CodeExists)
        return;

    uint8_t* pc = (uint8_t*)get_pc();

    const wasm::ModuleSegment* ms = nullptr;
    if (!wasm::InInterruptibleCode(cx_, pc, &ms))
        return;

    if (!cx_->activation()->asJit()->startWasmInterrupt(registerState()))
        return;

    set_pc(int32_t(ms->interruptCode()));
}

static inline JitActivation*
GetJitActivation(JSContext* cx)
{
    if (!wasm::CodeExists)
        return nullptr;
    if (!cx->activation() || !cx->activation()->isJit())
        return nullptr;
    return cx->activation()->asJit();
}

// WebAssembly memories contain an extra region of guard pages (see
// WasmArrayRawBuffer comment). The guard pages catch out-of-bounds accesses
// using a signal handler that redirects PC to a stub that safely reports an
// error. However, if the handler is hit by the simulator, the PC is in C++ code
// and cannot be redirected. Therefore, we must avoid hitting the handler by
// redirecting in the simulator before the real handler would have been hit.
bool
Simulator::handleWasmSegFault(int32_t addr, unsigned numBytes)
{
    JitActivation* act = GetJitActivation(cx_);
    if (!act)
        return false;

    void* pc = reinterpret_cast<void*>(get_pc());
    uint8_t* fp = reinterpret_cast<uint8_t*>(get_register(r11));

    const wasm::CodeSegment* segment = wasm::LookupCodeSegment(pc);
    if (!segment || !segment->isModule())
        return false;
    const wasm::ModuleSegment* moduleSegment = segment->asModule();

    wasm::Instance* instance = wasm::LookupFaultingInstance(*moduleSegment, pc, fp);
    if (!instance)
        return false;

    MOZ_RELEASE_ASSERT(&instance->code() == &moduleSegment->code());

    if (!instance->memoryAccessInGuardRegion((uint8_t*)addr, numBytes))
        return false;

    const wasm::MemoryAccess* memoryAccess = instance->code().lookupMemoryAccess(pc);
    if (!memoryAccess) {
        MOZ_ALWAYS_TRUE(act->asJit()->startWasmInterrupt(registerState()));
        if (!instance->code().containsCodePC(pc))
            MOZ_CRASH("Cannot map PC to trap handler");
        set_pc(int32_t(moduleSegment->outOfBoundsCode()));
        return true;
    }

    MOZ_ASSERT(memoryAccess->hasTrapOutOfLineCode());
    set_pc(int32_t(memoryAccess->trapOutOfLineCode(moduleSegment->base())));
    return true;
}

bool
Simulator::handleWasmIllFault()
{
    JitActivation* act = GetJitActivation(cx_);
    if (!act)
        return false;

    void* pc = reinterpret_cast<void*>(get_pc());

    const wasm::CodeSegment* segment = wasm::LookupCodeSegment(pc);
    if (!segment || !segment->isModule())
        return false;
    const wasm::ModuleSegment* moduleSegment = segment->asModule();

    wasm::Trap trap;
    wasm::BytecodeOffset bytecode;
    if (!moduleSegment->code().lookupTrap(pc, &trap, &bytecode))
        return false;

    act->startWasmTrap(trap, bytecode.offset, registerState());
    set_pc(int32_t(moduleSegment->trapCode()));
    return true;
}

uint64_t
Simulator::readQ(int32_t addr, SimInstruction* instr, UnalignedPolicy f)
{
    if (handleWasmSegFault(addr, 8))
        return UINT64_MAX;

    if ((addr & 3) == 0 || (f == AllowUnaligned && !HasAlignmentFault())) {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(addr);
        return *ptr;
    }

    // See the comments below in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        uint64_t value;
        memcpy(&value, ptr, sizeof(value));
        return value;
    }

    printf("Unaligned read at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
}

void
Simulator::writeQ(int32_t addr, uint64_t value, SimInstruction* instr, UnalignedPolicy f)
{
    if (handleWasmSegFault(addr, 8))
        return;

    if ((addr & 3) == 0 || (f == AllowUnaligned && !HasAlignmentFault())) {
        uint64_t* ptr = reinterpret_cast<uint64_t*>(addr);
        *ptr = value;
        return;
    }

    // See the comments below in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        memcpy(ptr, &value, sizeof(value));
        return;
    }

    printf("Unaligned write at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
}

int
Simulator::readW(int32_t addr, SimInstruction* instr, UnalignedPolicy f)
{
    if (handleWasmSegFault(addr, 4))
        return -1;

    if ((addr & 3) == 0 || (f == AllowUnaligned && !HasAlignmentFault())) {
        intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
        return *ptr;
    }

    // In WebAssembly, we want unaligned accesses to either raise a signal or
    // do the right thing. Making this simulator properly emulate the behavior
    // of raising a signal is complex, so as a special-case, when in wasm code,
    // we just do the right thing.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        int value;
        memcpy(&value, ptr, sizeof(value));
        return value;
    }

    printf("Unaligned read at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
}

void
Simulator::writeW(int32_t addr, int value, SimInstruction* instr, UnalignedPolicy f)
{
    if (handleWasmSegFault(addr, 4))
        return;

    if ((addr & 3) == 0 || (f == AllowUnaligned && !HasAlignmentFault())) {
        intptr_t* ptr = reinterpret_cast<intptr_t*>(addr);
        *ptr = value;
        return;
    }

    // See the comments above in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        memcpy(ptr, &value, sizeof(value));
        return;
    }

    printf("Unaligned write at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
}

// For the time being, define Relaxed operations in terms of SeqCst
// operations - we don't yet need Relaxed operations anywhere else in
// the system, and the distinction is not important to the simulation
// at the level where we're operating.

template<typename T>
static
T loadRelaxed(SharedMem<T*> addr)
{
    return AtomicOperations::loadSeqCst(addr);
}

template<typename T>
static
T compareExchangeRelaxed(SharedMem<T*> addr, T oldval, T newval)
{
    return AtomicOperations::compareExchangeSeqCst(addr, oldval, newval);
}

int
Simulator::readExW(int32_t addr, SimInstruction* instr)
{
    if (addr & 3)
        MOZ_CRASH("Unaligned exclusive read");

    if (handleWasmSegFault(addr, 4))
        return -1;

    SharedMem<int32_t*> ptr = SharedMem<int32_t*>::shared(reinterpret_cast<int32_t*>(addr));
    int32_t value = loadRelaxed(ptr);
    exclusiveMonitorSet(value);
    return value;
}

int32_t
Simulator::writeExW(int32_t addr, int value, SimInstruction* instr)
{
    if (addr & 3)
        MOZ_CRASH("Unaligned exclusive write");

    if (handleWasmSegFault(addr, 4))
        return -1;

    SharedMem<int32_t*> ptr = SharedMem<int32_t*>::shared(reinterpret_cast<int32_t*>(addr));
    bool held;
    int32_t expected = int32_t(exclusiveMonitorGetAndClear(&held));
    if (!held)
        return 1;
    int32_t old = compareExchangeRelaxed(ptr, expected, int32_t(value));
    return old != expected;
}

uint16_t
Simulator::readHU(int32_t addr, SimInstruction* instr)
{
    if (handleWasmSegFault(addr, 2))
        return UINT16_MAX;

    // The regexp engine emits unaligned loads, so we don't check for them here
    // like most of the other methods do.
    if ((addr & 1) == 0 || !HasAlignmentFault()) {
        uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
        return *ptr;
    }

    // See comments above in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        uint16_t value;
        memcpy(&value, ptr, sizeof(value));
        return value;
    }

    printf("Unaligned unsigned halfword read at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
    return 0;
}

int16_t
Simulator::readH(int32_t addr, SimInstruction* instr)
{
    if (handleWasmSegFault(addr, 2))
        return -1;

    if ((addr & 1) == 0 || !HasAlignmentFault()) {
        int16_t* ptr = reinterpret_cast<int16_t*>(addr);
        return *ptr;
    }

    // See comments above in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        int16_t value;
        memcpy(&value, ptr, sizeof(value));
        return value;
    }

    printf("Unaligned signed halfword read at 0x%08x\n", addr);
    MOZ_CRASH();
    return 0;
}

void
Simulator::writeH(int32_t addr, uint16_t value, SimInstruction* instr)
{
    if (handleWasmSegFault(addr, 2))
        return;

    if ((addr & 1) == 0 || !HasAlignmentFault()) {
        uint16_t* ptr = reinterpret_cast<uint16_t*>(addr);
        *ptr = value;
        return;
    }

    // See the comments above in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        memcpy(ptr, &value, sizeof(value));
        return;
    }

    printf("Unaligned unsigned halfword write at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
}

void
Simulator::writeH(int32_t addr, int16_t value, SimInstruction* instr)
{
    if (handleWasmSegFault(addr, 2))
        return;

    if ((addr & 1) == 0 || !HasAlignmentFault()) {
        int16_t* ptr = reinterpret_cast<int16_t*>(addr);
        *ptr = value;
        return;
    }

    // See the comments above in readW.
    if (FixupFault() && wasm::InCompiledCode(reinterpret_cast<void*>(get_pc()))) {
        char* ptr = reinterpret_cast<char*>(addr);
        memcpy(ptr, &value, sizeof(value));
        return;
    }

    printf("Unaligned halfword write at 0x%08x, pc=%p\n", addr, instr);
    MOZ_CRASH();
}

uint16_t
Simulator::readExHU(int32_t addr, SimInstruction* instr)
{
    if (addr & 1)
        MOZ_CRASH("Unaligned exclusive read");

    if (handleWasmSegFault(addr, 2))
        return UINT16_MAX;

    SharedMem<uint16_t*> ptr = SharedMem<uint16_t*>::shared(reinterpret_cast<uint16_t*>(addr));
    uint16_t value = loadRelaxed(ptr);
    exclusiveMonitorSet(value);
    return value;
}

int32_t
Simulator::writeExH(int32_t addr, uint16_t value, SimInstruction* instr)
{
    if (addr & 1)
        MOZ_CRASH("Unaligned exclusive write");

    if (handleWasmSegFault(addr, 2))
        return -1;

    SharedMem<uint16_t*> ptr = SharedMem<uint16_t*>::shared(reinterpret_cast<uint16_t*>(addr));
    bool held;
    uint16_t expected = uint16_t(exclusiveMonitorGetAndClear(&held));
    if (!held)
        return 1;
    uint16_t old = compareExchangeRelaxed(ptr, expected, value);
    return old != expected;
}

uint8_t
Simulator::readBU(int32_t addr)
{
    if (handleWasmSegFault(addr, 1))
        return UINT8_MAX;

    uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
    return *ptr;
}

uint8_t
Simulator::readExBU(int32_t addr)
{
    if (handleWasmSegFault(addr, 1))
        return UINT8_MAX;

    SharedMem<uint8_t*> ptr = SharedMem<uint8_t*>::shared(reinterpret_cast<uint8_t*>(addr));
    uint8_t value = loadRelaxed(ptr);
    exclusiveMonitorSet(value);
    return value;
}

int32_t
Simulator::writeExB(int32_t addr, uint8_t value)
{
    if (handleWasmSegFault(addr, 1))
        return -1;

    SharedMem<uint8_t*> ptr = SharedMem<uint8_t*>::shared(reinterpret_cast<uint8_t*>(addr));
    bool held;
    uint8_t expected = uint8_t(exclusiveMonitorGetAndClear(&held));
    if (!held)
        return 1;
    uint8_t old = compareExchangeRelaxed(ptr, expected, value);
    return old != expected;
}

int8_t
Simulator::readB(int32_t addr)
{
    if (handleWasmSegFault(addr, 1))
        return -1;

    int8_t* ptr = reinterpret_cast<int8_t*>(addr);
    return *ptr;
}

void
Simulator::writeB(int32_t addr, uint8_t value)
{
    if (handleWasmSegFault(addr, 1))
        return;

    uint8_t* ptr = reinterpret_cast<uint8_t*>(addr);
    *ptr = value;
}

void
Simulator::writeB(int32_t addr, int8_t value)
{
    if (handleWasmSegFault(addr, 1))
        return;

    int8_t* ptr = reinterpret_cast<int8_t*>(addr);
    *ptr = value;
}

int32_t*
Simulator::readDW(int32_t addr)
{
    if (handleWasmSegFault(addr, 8))
        return nullptr;

    if ((addr & 3) == 0) {
        int32_t* ptr = reinterpret_cast<int32_t*>(addr);
        return ptr;
    }

    printf("Unaligned read at 0x%08x\n", addr);
    MOZ_CRASH();
}

void
Simulator::writeDW(int32_t addr, int32_t value1, int32_t value2)
{
    if (handleWasmSegFault(addr, 8))
        return;

    if ((addr & 3) == 0) {
        int32_t* ptr = reinterpret_cast<int32_t*>(addr);
        *ptr++ = value1;
        *ptr = value2;
        return;
    }

    printf("Unaligned write at 0x%08x\n", addr);
    MOZ_CRASH();
}

int32_t
Simulator::readExDW(int32_t addr, int32_t* hibits)
{
    if (addr & 3)
        MOZ_CRASH("Unaligned exclusive read");

    if (handleWasmSegFault(addr, 8))
        return -1;

    SharedMem<uint64_t*> ptr = SharedMem<uint64_t*>::shared(reinterpret_cast<uint64_t*>(addr));
    // The spec says that the low part of value shall be read from addr and
    // the high part shall be read from addr+4.  On a little-endian system
    // where we read a 64-bit quadword the low part of the value will be in
    // the low part of the quadword, and the high part of the value in the
    // high part of the quadword.
    uint64_t value = loadRelaxed(ptr);
    exclusiveMonitorSet(value);
    *hibits = int32_t(value >> 32);
    return int32_t(value);
}

int32_t
Simulator::writeExDW(int32_t addr, int32_t value1, int32_t value2)
{
    if (addr & 3)
        MOZ_CRASH("Unaligned exclusive write");

    if (handleWasmSegFault(addr, 8))
        return -1;

    SharedMem<uint64_t*> ptr = SharedMem<uint64_t*>::shared(reinterpret_cast<uint64_t*>(addr));
    // The spec says that value1 shall be stored at addr and value2 at
    // addr+4.  On a little-endian system that means constructing a 64-bit
    // value where value1 is in the low half of a 64-bit quadword and value2
    // is in the high half of the quadword.
    uint64_t value = (uint64_t(value2) << 32) | uint32_t(value1);
    bool held;
    uint64_t expected = exclusiveMonitorGetAndClear(&held);
    if (!held)
        return 1;
    uint64_t old = compareExchangeRelaxed(ptr, expected, value);
    return old != expected;
}

uintptr_t
Simulator::stackLimit() const
{
    return stackLimit_;
}

uintptr_t*
Simulator::addressOfStackLimit()
{
    return &stackLimit_;
}

bool
Simulator::overRecursed(uintptr_t newsp) const
{
    if (newsp == 0)
        newsp = get_register(sp);
    return newsp <= stackLimit();
}

bool
Simulator::overRecursedWithExtra(uint32_t extra) const
{
    uintptr_t newsp = get_register(sp) - extra;
    return newsp <= stackLimit();
}

// Checks if the current instruction should be executed based on its condition
// bits.
bool
Simulator::conditionallyExecute(SimInstruction* instr)
{
    switch (instr->conditionField()) {
      case Assembler::EQ: return z_flag_;
      case Assembler::NE: return !z_flag_;
      case Assembler::CS: return c_flag_;
      case Assembler::CC: return !c_flag_;
      case Assembler::MI: return n_flag_;
      case Assembler::PL: return !n_flag_;
      case Assembler::VS: return v_flag_;
      case Assembler::VC: return !v_flag_;
      case Assembler::HI: return c_flag_ && !z_flag_;
      case Assembler::LS: return !c_flag_ || z_flag_;
      case Assembler::GE: return n_flag_ == v_flag_;
      case Assembler::LT: return n_flag_ != v_flag_;
      case Assembler::GT: return !z_flag_ && (n_flag_ == v_flag_);
      case Assembler::LE: return z_flag_ || (n_flag_ != v_flag_);
      case Assembler::AL: return true;
      default: MOZ_CRASH();
    }
    return false;
}

// Calculate and set the Negative and Zero flags.
void
Simulator::setNZFlags(int32_t val)
{
    n_flag_ = (val < 0);
    z_flag_ = (val == 0);
}

// Set the Carry flag.
void
Simulator::setCFlag(bool val)
{
    c_flag_ = val;
}

// Set the oVerflow flag.
void
Simulator::setVFlag(bool val)
{
    v_flag_ = val;
}

// Calculate C flag value for additions.
bool
Simulator::carryFrom(int32_t left, int32_t right, int32_t carry)
{
    uint32_t uleft = static_cast<uint32_t>(left);
    uint32_t uright = static_cast<uint32_t>(right);
    uint32_t urest  = 0xffffffffU - uleft;
    return (uright > urest) ||
           (carry && (((uright + 1) > urest) || (uright > (urest - 1))));
}

// Calculate C flag value for subtractions.
bool
Simulator::borrowFrom(int32_t left, int32_t right)
{
    uint32_t uleft = static_cast<uint32_t>(left);
    uint32_t uright = static_cast<uint32_t>(right);
    return (uright > uleft);
}

// Calculate V flag value for additions and subtractions.
bool
Simulator::overflowFrom(int32_t alu_out, int32_t left, int32_t right, bool addition)
{
    bool overflow;
    if (addition) {
        // Operands have the same sign.
        overflow = ((left >= 0 && right >= 0) || (left < 0 && right < 0))
            // And operands and result have different sign.
            && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
    } else {
        // Operands have different signs.
        overflow = ((left < 0 && right >= 0) || (left >= 0 && right < 0))
            // And first operand and result have different signs.
            && ((left < 0 && alu_out >= 0) || (left >= 0 && alu_out < 0));
    }
    return overflow;
}

// Support for VFP comparisons.
void
Simulator::compute_FPSCR_Flags(double val1, double val2)
{
    if (mozilla::IsNaN(val1) || mozilla::IsNaN(val2)) {
        n_flag_FPSCR_ = false;
        z_flag_FPSCR_ = false;
        c_flag_FPSCR_ = true;
        v_flag_FPSCR_ = true;
        // All non-NaN cases.
    } else if (val1 == val2) {
        n_flag_FPSCR_ = false;
        z_flag_FPSCR_ = true;
        c_flag_FPSCR_ = true;
        v_flag_FPSCR_ = false;
    } else if (val1 < val2) {
        n_flag_FPSCR_ = true;
        z_flag_FPSCR_ = false;
        c_flag_FPSCR_ = false;
        v_flag_FPSCR_ = false;
    } else {
        // Case when (val1 > val2).
        n_flag_FPSCR_ = false;
        z_flag_FPSCR_ = false;
        c_flag_FPSCR_ = true;
        v_flag_FPSCR_ = false;
    }
}

void
Simulator::copy_FPSCR_to_APSR()
{
    n_flag_ = n_flag_FPSCR_;
    z_flag_ = z_flag_FPSCR_;
    c_flag_ = c_flag_FPSCR_;
    v_flag_ = v_flag_FPSCR_;
}

// Addressing Mode 1 - Data-processing operands:
// Get the value based on the shifter_operand with register.
int32_t
Simulator::getShiftRm(SimInstruction* instr, bool* carry_out)
{
    ShiftType shift = instr->shifttypeValue();
    int shift_amount = instr->shiftAmountValue();
    int32_t result = get_register(instr->rmValue());
    if (instr->bit(4) == 0) {
        // By immediate.
        if (shift == ROR && shift_amount == 0) {
            MOZ_CRASH("NYI");
            return result;
        }
        if ((shift == LSR || shift == ASR) && shift_amount == 0)
            shift_amount = 32;
        switch (shift) {
          case ASR: {
            if (shift_amount == 0) {
                if (result < 0) {
                    result = 0xffffffff;
                    *carry_out = true;
                } else {
                    result = 0;
                    *carry_out = false;
                }
            } else {
                result >>= (shift_amount - 1);
                *carry_out = (result & 1) == 1;
                result >>= 1;
            }
            break;
          }

          case LSL: {
            if (shift_amount == 0) {
                *carry_out = c_flag_;
            } else {
                result <<= (shift_amount - 1);
                *carry_out = (result < 0);
                result <<= 1;
            }
            break;
          }

          case LSR: {
            if (shift_amount == 0) {
                result = 0;
                *carry_out = c_flag_;
            } else {
                uint32_t uresult = static_cast<uint32_t>(result);
                uresult >>= (shift_amount - 1);
                *carry_out = (uresult & 1) == 1;
                uresult >>= 1;
                result = static_cast<int32_t>(uresult);
            }
            break;
          }

          case ROR: {
            if (shift_amount == 0) {
                *carry_out = c_flag_;
            } else {
                uint32_t left = static_cast<uint32_t>(result) >> shift_amount;
                uint32_t right = static_cast<uint32_t>(result) << (32 - shift_amount);
                result = right | left;
                *carry_out = (static_cast<uint32_t>(result) >> 31) != 0;
            }
            break;
          }

          default:
            MOZ_CRASH();
        }
    } else {
        // By register.
        int rs = instr->rsValue();
        shift_amount = get_register(rs) &0xff;
        switch (shift) {
          case ASR: {
            if (shift_amount == 0) {
                *carry_out = c_flag_;
            } else if (shift_amount < 32) {
                result >>= (shift_amount - 1);
                *carry_out = (result & 1) == 1;
                result >>= 1;
            } else {
                MOZ_ASSERT(shift_amount >= 32);
                if (result < 0) {
                    *carry_out = true;
                    result = 0xffffffff;
                } else {
                    *carry_out = false;
                    result = 0;
                }
            }
            break;
          }

          case LSL: {
            if (shift_amount == 0) {
                *carry_out = c_flag_;
            } else if (shift_amount < 32) {
                result <<= (shift_amount - 1);
                *carry_out = (result < 0);
                result <<= 1;
            } else if (shift_amount == 32) {
                *carry_out = (result & 1) == 1;
                result = 0;
            } else {
                MOZ_ASSERT(shift_amount > 32);
                *carry_out = false;
                result = 0;
            }
            break;
          }

          case LSR: {
            if (shift_amount == 0) {
                *carry_out = c_flag_;
            } else if (shift_amount < 32) {
                uint32_t uresult = static_cast<uint32_t>(result);
                uresult >>= (shift_amount - 1);
                *carry_out = (uresult & 1) == 1;
                uresult >>= 1;
                result = static_cast<int32_t>(uresult);
            } else if (shift_amount == 32) {
                *carry_out = (result < 0);
                result = 0;
            } else {
                *carry_out = false;
                result = 0;
            }
            break;
          }

          case ROR: {
            if (shift_amount == 0) {
                *carry_out = c_flag_;
            } else {
                uint32_t left = static_cast<uint32_t>(result) >> shift_amount;
                uint32_t right = static_cast<uint32_t>(result) << (32 - shift_amount);
                result = right | left;
                *carry_out = (static_cast<uint32_t>(result) >> 31) != 0;
            }
            break;
          }

          default:
            MOZ_CRASH();
        }
    }
    return result;
}

// Addressing Mode 1 - Data-processing operands:
// Get the value based on the shifter_operand with immediate.
int32_t
Simulator::getImm(SimInstruction* instr, bool* carry_out)
{
    int rotate = instr->rotateValue() * 2;
    int immed8 = instr->immed8Value();
    int imm = (immed8 >> rotate) | (immed8 << (32 - rotate));
    *carry_out = (rotate == 0) ? c_flag_ : (imm < 0);
    return imm;
}

int32_t
Simulator::processPU(SimInstruction* instr, int num_regs, int reg_size,
                     intptr_t* start_address, intptr_t* end_address)
{
    int rn = instr->rnValue();
    int32_t rn_val = get_register(rn);
    switch (instr->PUField()) {
      case da_x:
        MOZ_CRASH();
        break;
      case ia_x:
        *start_address = rn_val;
        *end_address = rn_val + (num_regs * reg_size) - reg_size;
        rn_val = rn_val + (num_regs * reg_size);
        break;
      case db_x:
        *start_address = rn_val - (num_regs * reg_size);
        *end_address = rn_val - reg_size;
        rn_val = *start_address;
        break;
      case ib_x:
        *start_address = rn_val + reg_size;
        *end_address = rn_val + (num_regs * reg_size);
        rn_val = *end_address;
        break;
      default:
        MOZ_CRASH();
    }
    return rn_val;
}

// Addressing Mode 4 - Load and Store Multiple
void
Simulator::handleRList(SimInstruction* instr, bool load)
{
    int rlist = instr->rlistValue();
    int num_regs = mozilla::CountPopulation32(rlist);

    intptr_t start_address = 0;
    intptr_t end_address = 0;
    int32_t rn_val = processPU(instr, num_regs, sizeof(void*), &start_address, &end_address);
    intptr_t* address = reinterpret_cast<intptr_t*>(start_address);

    // Catch null pointers a little earlier.
    MOZ_ASSERT(start_address > 8191 || start_address < 0);

    int reg = 0;
    while (rlist != 0) {
        if ((rlist & 1) != 0) {
            if (load) {
                set_register(reg, *address);
            } else {
                *address = get_register(reg);
            }
            address += 1;
        }
        reg++;
        rlist >>= 1;
    }
    MOZ_ASSERT(end_address == ((intptr_t)address) - 4);
    if (instr->hasW())
        set_register(instr->rnValue(), rn_val);
}

// Addressing Mode 6 - Load and Store Multiple Coprocessor registers.
void
Simulator::handleVList(SimInstruction* instr)
{
    VFPRegPrecision precision = (instr->szValue() == 0) ? kSinglePrecision : kDoublePrecision;
    int operand_size = (precision == kSinglePrecision) ? 4 : 8;
    bool load = (instr->VLValue() == 0x1);

    int vd;
    int num_regs;
    vd = instr->VFPDRegValue(precision);
    if (precision == kSinglePrecision)
        num_regs = instr->immed8Value();
    else
        num_regs = instr->immed8Value() / 2;

    intptr_t start_address = 0;
    intptr_t end_address = 0;
    int32_t rn_val = processPU(instr, num_regs, operand_size, &start_address, &end_address);

    intptr_t* address = reinterpret_cast<intptr_t*>(start_address);
    for (int reg = vd; reg < vd + num_regs; reg++) {
        if (precision == kSinglePrecision) {
            if (load)
                set_s_register_from_sinteger(reg, readW(reinterpret_cast<int32_t>(address), instr));
            else
                writeW(reinterpret_cast<int32_t>(address), get_sinteger_from_s_register(reg), instr);
            address += 1;
        } else {
            if (load) {
                int32_t data[] = {
                    readW(reinterpret_cast<int32_t>(address), instr),
                    readW(reinterpret_cast<int32_t>(address + 1), instr)
                };
                double d;
                memcpy(&d, data, 8);
                set_d_register_from_double(reg, d);
            } else {
                int32_t data[2];
                double d;
                get_double_from_d_register(reg, &d);
                memcpy(data, &d, 8);
                writeW(reinterpret_cast<int32_t>(address), data[0], instr);
                writeW(reinterpret_cast<int32_t>(address + 1), data[1], instr);
            }
            address += 2;
        }
    }
    MOZ_ASSERT(reinterpret_cast<intptr_t>(address) - operand_size == end_address);
    if (instr->hasW())
        set_register(instr->rnValue(), rn_val);
}


// Note: With the code below we assume that all runtime calls return a 64 bits
// result. If they don't, the r1 result register contains a bogus value, which
// is fine because it is caller-saved.
typedef int64_t (*Prototype_General0)();
typedef int64_t (*Prototype_General1)(int32_t arg0);
typedef int64_t (*Prototype_General2)(int32_t arg0, int32_t arg1);
typedef int64_t (*Prototype_General3)(int32_t arg0, int32_t arg1, int32_t arg2);
typedef int64_t (*Prototype_General4)(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3);
typedef int64_t (*Prototype_General5)(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                                      int32_t arg4);
typedef int64_t (*Prototype_General6)(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                                      int32_t arg4, int32_t arg5);
typedef int64_t (*Prototype_General7)(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                                      int32_t arg4, int32_t arg5, int32_t arg6);
typedef int64_t (*Prototype_General8)(int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3,
                                      int32_t arg4, int32_t arg5, int32_t arg6, int32_t arg7);
typedef int64_t (*Prototype_GeneralGeneralGeneralInt64)(int32_t arg0, int32_t arg1, int32_t arg2,
                                                        int64_t arg3);
typedef int64_t (*Prototype_GeneralGeneralInt64Int64)(int32_t arg0, int32_t arg1, int64_t arg2,
                                                      int64_t arg3);

typedef double (*Prototype_Double_None)();
typedef double (*Prototype_Double_Double)(double arg0);
typedef double (*Prototype_Double_Int)(int32_t arg0);
typedef double (*Prototype_Double_IntInt)(int32_t arg0, int32_t arg1);
typedef int32_t (*Prototype_Int_Double)(double arg0);
typedef int64_t (*Prototype_Int64_Double)(double arg0);
typedef int32_t (*Prototype_Int_DoubleIntInt)(double arg0, int32_t arg1, int32_t arg2);
typedef int32_t (*Prototype_Int_IntDoubleIntInt)(int32_t arg0, double arg1, int32_t arg2,
                                                 int32_t arg3);
typedef float (*Prototype_Float32_Float32)(float arg0);
typedef float (*Prototype_Float32_Float32Float32)(float arg0, float arg1);
typedef float (*Prototype_Float32_IntInt)(int arg0, int arg1);

typedef double (*Prototype_DoubleInt)(double arg0, int32_t arg1);
typedef double (*Prototype_Double_IntDouble)(int32_t arg0, double arg1);
typedef double (*Prototype_Double_DoubleDouble)(double arg0, double arg1);
typedef int32_t (*Prototype_Int_IntDouble)(int32_t arg0, double arg1);

typedef double (*Prototype_Double_DoubleDoubleDouble)(double arg0, double arg1, double arg2);
typedef double (*Prototype_Double_DoubleDoubleDoubleDouble)(double arg0, double arg1,
                                                            double arg2, double arg3);

// Fill the volatile registers with scratch values.
//
// Some of the ABI calls assume that the float registers are not scratched, even
// though the ABI defines them as volatile - a performance optimization. These
// are all calls passing operands in integer registers, so for now the simulator
// does not scratch any float registers for these calls. Should try to narrow it
// further in future.
//
void
Simulator::scratchVolatileRegisters(bool scratchFloat)
{
    int32_t scratch_value = 0xa5a5a5a5 ^ uint32_t(icount_);
    set_register(r0, scratch_value);
    set_register(r1, scratch_value);
    set_register(r2, scratch_value);
    set_register(r3, scratch_value);
    set_register(r12, scratch_value); // Intra-Procedure-call scratch register.
    set_register(r14, scratch_value); // Link register.

    if (scratchFloat) {
        uint64_t scratch_value_d = 0x5a5a5a5a5a5a5a5aLU ^ uint64_t(icount_) ^ (uint64_t(icount_) << 30);
        for (uint32_t i = d0; i < d8; i++)
            set_d_register(i, &scratch_value_d);
        for (uint32_t i = d16; i < FloatRegisters::TotalPhys; i++)
            set_d_register(i, &scratch_value_d);
    }
}

static int64_t
MakeInt64(int32_t first, int32_t second)
{
    // Little-endian order.
    return ((int64_t)second << 32) | (uint32_t)first;
}

// Software interrupt instructions are used by the simulator to call into C++.
void
Simulator::softwareInterrupt(SimInstruction* instr)
{
    int svc = instr->svcValue();
    switch (svc) {
      case kCallRtRedirected: {
        Redirection* redirection = Redirection::FromSwiInstruction(instr);
        int32_t arg0 = get_register(r0);
        int32_t arg1 = get_register(r1);
        int32_t arg2 = get_register(r2);
        int32_t arg3 = get_register(r3);
        int32_t* stack_pointer = reinterpret_cast<int32_t*>(get_register(sp));
        int32_t arg4 = stack_pointer[0];
        int32_t arg5 = stack_pointer[1];

        int32_t saved_lr = get_register(lr);
        intptr_t external = reinterpret_cast<intptr_t>(redirection->nativeFunction());

        bool stack_aligned = (get_register(sp) & (ABIStackAlignment - 1)) == 0;
        if (!stack_aligned) {
            fprintf(stderr, "Runtime call with unaligned stack!\n");
            MOZ_CRASH();
        }

        if (single_stepping_)
            single_step_callback_(single_step_callback_arg_, this, nullptr);

        switch (redirection->type()) {
          case Args_General0: {
            Prototype_General0 target = reinterpret_cast<Prototype_General0>(external);
            int64_t result = target();
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_General1: {
            Prototype_General1 target = reinterpret_cast<Prototype_General1>(external);
            int64_t result = target(arg0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_General2: {
            Prototype_General2 target = reinterpret_cast<Prototype_General2>(external);
            int64_t result = target(arg0, arg1);
            // The ARM backend makes calls to __aeabi_idivmod and
            // __aeabi_uidivmod assuming that the float registers are
            // non-volatile as a performance optimization, so the float
            // registers must not be scratch when calling these.
            bool scratchFloat = target != __aeabi_idivmod && target != __aeabi_uidivmod;
            scratchVolatileRegisters(/* scratchFloat = */ scratchFloat);
            setCallResult(result);
            break;
          }
          case Args_General3: {
            Prototype_General3 target = reinterpret_cast<Prototype_General3>(external);
            int64_t result = target(arg0, arg1, arg2);
            scratchVolatileRegisters(/* scratchFloat = true*/);
            setCallResult(result);
            break;
          }
          case Args_General4: {
            Prototype_General4 target = reinterpret_cast<Prototype_General4>(external);
            int64_t result = target(arg0, arg1, arg2, arg3);
            scratchVolatileRegisters(/* scratchFloat = true*/);
            setCallResult(result);
            break;
          }
          case Args_General5: {
            Prototype_General5 target = reinterpret_cast<Prototype_General5>(external);
            int64_t result = target(arg0, arg1, arg2, arg3, arg4);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_General6: {
            Prototype_General6 target = reinterpret_cast<Prototype_General6>(external);
            int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_General7: {
            Prototype_General7 target = reinterpret_cast<Prototype_General7>(external);
            int32_t arg6 = stack_pointer[2];
            int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5, arg6);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_General8: {
            Prototype_General8 target = reinterpret_cast<Prototype_General8>(external);
            int32_t arg6 = stack_pointer[2];
            int32_t arg7 = stack_pointer[3];
            int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_Int_GeneralGeneralGeneralInt64: {
            Prototype_GeneralGeneralGeneralInt64 target =
                reinterpret_cast<Prototype_GeneralGeneralGeneralInt64>(external);
            // The int64 arg is not split across register and stack
            int64_t result = target(arg0, arg1, arg2, MakeInt64(arg4, arg5));
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_Int_GeneralGeneralInt64Int64: {
            Prototype_GeneralGeneralInt64Int64 target =
                reinterpret_cast<Prototype_GeneralGeneralInt64Int64>(external);
            int64_t result = target(arg0, arg1, MakeInt64(arg2, arg3), MakeInt64(arg4, arg5));
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_Int64_Double: {
            double dval0, dval1;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            Prototype_Int64_Double target = reinterpret_cast<Prototype_Int64_Double>(external);
            int64_t result = target(dval0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResult(result);
            break;
          }
          case Args_Double_None: {
            Prototype_Double_None target = reinterpret_cast<Prototype_Double_None>(external);
            double dresult = target();
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Int_Double: {
            double dval0, dval1;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            Prototype_Int_Double target = reinterpret_cast<Prototype_Int_Double>(external);
            int32_t res = target(dval0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            set_register(r0, res);
            break;
          }
          case Args_Double_Double: {
            double dval0, dval1;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            Prototype_Double_Double target = reinterpret_cast<Prototype_Double_Double>(external);
            double dresult = target(dval0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Float32_Float32: {
            float fval0;
            if (UseHardFpABI())
                get_float_from_s_register(0, &fval0);
            else
                fval0 = mozilla::BitwiseCast<float>(arg0);
            Prototype_Float32_Float32 target = reinterpret_cast<Prototype_Float32_Float32>(external);
            float fresult = target(fval0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultFloat(fresult);
            break;
          }
          case Args_Float32_Float32Float32: {
            float fval0, fval1;
            if (UseHardFpABI()) {
                get_float_from_s_register(0, &fval0);
                get_float_from_s_register(1, &fval1);
            } else {
                fval0 = mozilla::BitwiseCast<float>(arg0);
                fval1 = mozilla::BitwiseCast<float>(arg1);
            }
            Prototype_Float32_Float32Float32 target = reinterpret_cast<Prototype_Float32_Float32Float32>(external);
            float fresult = target(fval0, fval1);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultFloat(fresult);
            break;
          }
          case Args_Float32_IntInt: {
            Prototype_Float32_IntInt target = reinterpret_cast<Prototype_Float32_IntInt>(external);
            float fresult = target(arg0, arg1);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultFloat(fresult);
            break;
          }
          case Args_Double_Int: {
            Prototype_Double_Int target = reinterpret_cast<Prototype_Double_Int>(external);
            double dresult = target(arg0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Double_IntInt: {
            Prototype_Double_IntInt target = reinterpret_cast<Prototype_Double_IntInt>(external);
            double dresult = target(arg0, arg1);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Double_DoubleInt: {
            double dval0, dval1;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            Prototype_DoubleInt target = reinterpret_cast<Prototype_DoubleInt>(external);
            double dresult = target(dval0, ival);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Double_DoubleDouble: {
            double dval0, dval1;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            Prototype_Double_DoubleDouble target = reinterpret_cast<Prototype_Double_DoubleDouble>(external);
            double dresult = target(dval0, dval1);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Double_IntDouble: {
            int32_t ival = get_register(0);
            double dval0;
            if (UseHardFpABI())
                get_double_from_d_register(0, &dval0);
            else
                dval0 = get_double_from_register_pair(2);
            Prototype_Double_IntDouble target = reinterpret_cast<Prototype_Double_IntDouble>(external);
            double dresult = target(ival, dval0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          case Args_Int_IntDouble: {
            int32_t ival = get_register(0);
            double dval0;
            if (UseHardFpABI())
                get_double_from_d_register(0, &dval0);
            else
                dval0 = get_double_from_register_pair(2);
            Prototype_Int_IntDouble target = reinterpret_cast<Prototype_Int_IntDouble>(external);
            int32_t result = target(ival, dval0);
            scratchVolatileRegisters(/* scratchFloat = true */);
            set_register(r0, result);
            break;
          }
          case Args_Int_DoubleIntInt: {
            double dval;
            int32_t result;
            Prototype_Int_DoubleIntInt target = reinterpret_cast<Prototype_Int_DoubleIntInt>(external);
            if (UseHardFpABI()) {
                get_double_from_d_register(0, &dval);
                result = target(dval, arg0, arg1);
            } else {
                dval = get_double_from_register_pair(0);
                result = target(dval, arg2, arg3);
            }
            scratchVolatileRegisters(/* scratchFloat = true */);
            set_register(r0, result);
            break;
          }
          case Args_Int_IntDoubleIntInt: {
            double dval;
            int32_t result;
            Prototype_Int_IntDoubleIntInt target = reinterpret_cast<Prototype_Int_IntDoubleIntInt>(external);
            if (UseHardFpABI()) {
                get_double_from_d_register(0, &dval);
                result = target(arg0, dval, arg1, arg2);
            } else {
                dval = get_double_from_register_pair(2);
                result = target(arg0, dval, arg4, arg5);
            }
            scratchVolatileRegisters(/* scratchFloat = true */);
            set_register(r0, result);
            break;
          }
          case Args_Double_DoubleDoubleDouble: {
            double dval0, dval1, dval2;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            // the last argument is on stack
            getFpFromStack(stack_pointer, &dval2);
            Prototype_Double_DoubleDoubleDouble target = reinterpret_cast<Prototype_Double_DoubleDoubleDouble>(external);
            double dresult = target(dval0, dval1, dval2);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
         }
         case Args_Double_DoubleDoubleDoubleDouble: {
            double dval0, dval1, dval2, dval3;
            int32_t ival;
            getFpArgs(&dval0, &dval1, &ival);
            // the two last arguments are on stack
            getFpFromStack(stack_pointer, &dval2);
            getFpFromStack(stack_pointer + 2, &dval3);
            Prototype_Double_DoubleDoubleDoubleDouble target = reinterpret_cast<Prototype_Double_DoubleDoubleDoubleDouble>(external);
            double dresult = target(dval0, dval1, dval2, dval3);
            scratchVolatileRegisters(/* scratchFloat = true */);
            setCallResultDouble(dresult);
            break;
          }
          default:
            MOZ_CRASH("call");
        }

        if (single_stepping_)
            single_step_callback_(single_step_callback_arg_, this, nullptr);

        set_register(lr, saved_lr);
        set_pc(get_register(lr));
        break;
      }
      case kBreakpoint: {
        ArmDebugger dbg(this);
        dbg.debug();
        break;
      }
      default: { // Stop uses all codes greater than 1 << 23.
        if (svc >= (1 << 23)) {
            uint32_t code = svc & kStopCodeMask;
            if (isWatchedStop(code))
                increaseStopCounter(code);

            // Stop if it is enabled, otherwise go on jumping over the stop and
            // the message address.
            if (isEnabledStop(code)) {
                ArmDebugger dbg(this);
                dbg.stop(instr);
            } else {
                set_pc(get_pc() + 2 * SimInstruction::kInstrSize);
            }
        } else {
            // This is not a valid svc code.
            MOZ_CRASH();
            break;
        }
      }
    }
}

void
Simulator::canonicalizeNaN(double* value)
{
    if (!wasm::CodeExists && !wasm::LookupCodeSegment(get_pc_as<void*>()) && FPSCR_default_NaN_mode_)
        *value = JS::CanonicalizeNaN(*value);
}

void
Simulator::canonicalizeNaN(float* value)
{
    if (!wasm::CodeExists && !wasm::LookupCodeSegment(get_pc_as<void*>()) && FPSCR_default_NaN_mode_)
        *value = JS::CanonicalizeNaN(*value);
}

// Stop helper functions.
bool
Simulator::isStopInstruction(SimInstruction* instr)
{
    return (instr->bits(27, 24) == 0xF) && (instr->svcValue() >= kStopCode);
}

bool Simulator::isWatchedStop(uint32_t code)
{
    MOZ_ASSERT(code <= kMaxStopCode);
    return code < kNumOfWatchedStops;
}

bool
Simulator::isEnabledStop(uint32_t code)
{
    MOZ_ASSERT(code <= kMaxStopCode);
    // Unwatched stops are always enabled.
    return !isWatchedStop(code) || !(watched_stops_[code].count & kStopDisabledBit);
}

void
Simulator::enableStop(uint32_t code)
{
    MOZ_ASSERT(isWatchedStop(code));
    if (!isEnabledStop(code))
        watched_stops_[code].count &= ~kStopDisabledBit;
}

void
Simulator::disableStop(uint32_t code)
{
    MOZ_ASSERT(isWatchedStop(code));
    if (isEnabledStop(code))
        watched_stops_[code].count |= kStopDisabledBit;
}

void
Simulator::increaseStopCounter(uint32_t code)
{
    MOZ_ASSERT(code <= kMaxStopCode);
    MOZ_ASSERT(isWatchedStop(code));
    if ((watched_stops_[code].count & ~(1 << 31)) == 0x7fffffff) {
        printf("Stop counter for code %i has overflowed.\n"
               "Enabling this code and reseting the counter to 0.\n", code);
        watched_stops_[code].count = 0;
        enableStop(code);
    } else {
        watched_stops_[code].count++;
    }
}

// Print a stop status.
void
Simulator::printStopInfo(uint32_t code)
{
    MOZ_ASSERT(code <= kMaxStopCode);
    if (!isWatchedStop(code)) {
        printf("Stop not watched.");
    } else {
        const char* state = isEnabledStop(code) ? "Enabled" : "Disabled";
        int32_t count = watched_stops_[code].count & ~kStopDisabledBit;
        // Don't print the state of unused breakpoints.
        if (count != 0) {
            if (watched_stops_[code].desc) {
                printf("stop %i - 0x%x: \t%s, \tcounter = %i, \t%s\n",
                       code, code, state, count, watched_stops_[code].desc);
            } else {
                printf("stop %i - 0x%x: \t%s, \tcounter = %i\n",
                       code, code, state, count);
            }
        }
    }
}

// Instruction types 0 and 1 are both rolled into one function because they only
// differ in the handling of the shifter_operand.
void
Simulator::decodeType01(SimInstruction* instr)
{
    int type = instr->typeValue();
    if (type == 0 && instr->isSpecialType0()) {
        // Multiply instruction or extra loads and stores.
        if (instr->bits(7, 4) == 9) {
            if (instr->bit(24) == 0) {
                // Raw field decoding here. Multiply instructions have their Rd
                // in funny places.
                int rn = instr->rnValue();
                int rm = instr->rmValue();
                int rs = instr->rsValue();
                int32_t rs_val = get_register(rs);
                int32_t rm_val = get_register(rm);
                if (instr->bit(23) == 0) {
                    if (instr->bit(21) == 0) {
                        // The MUL instruction description (A 4.1.33) refers to
                        // Rd as being the destination for the operation, but it
                        // confusingly uses the Rn field to encode it.
                        int rd = rn;  // Remap the rn field to the Rd register.
                        int32_t alu_out = rm_val * rs_val;
                        set_register(rd, alu_out);
                        if (instr->hasS())
                            setNZFlags(alu_out);
                    } else {
                        int rd = instr->rdValue();
                        int32_t acc_value = get_register(rd);
                        if (instr->bit(22) == 0) {
                            // The MLA instruction description (A 4.1.28) refers
                            // to the order of registers as "Rd, Rm, Rs,
                            // Rn". But confusingly it uses the Rn field to
                            // encode the Rd register and the Rd field to encode
                            // the Rn register.
                            int32_t mul_out = rm_val * rs_val;
                            int32_t result = acc_value + mul_out;
                            set_register(rn, result);
                        } else {
                            int32_t mul_out = rm_val * rs_val;
                            int32_t result = acc_value - mul_out;
                            set_register(rn, result);
                        }
                    }
                } else {
                    // The signed/long multiply instructions use the terms RdHi
                    // and RdLo when referring to the target registers. They are
                    // mapped to the Rn and Rd fields as follows:
                    // RdLo == Rd
                    // RdHi == Rn (This is confusingly stored in variable rd here
                    //             because the mul instruction from above uses the
                    //             Rn field to encode the Rd register. Good luck figuring
                    //             this out without reading the ARM instruction manual
                    //             at a very detailed level.)
                    int rd_hi = rn;  // Remap the rn field to the RdHi register.
                    int rd_lo = instr->rdValue();
                    int32_t hi_res = 0;
                    int32_t lo_res = 0;
                    if (instr->bit(22) == 1) {
                        int64_t left_op  = static_cast<int32_t>(rm_val);
                        int64_t right_op = static_cast<int32_t>(rs_val);
                        uint64_t result = left_op * right_op;
                        hi_res = static_cast<int32_t>(result >> 32);
                        lo_res = static_cast<int32_t>(result & 0xffffffff);
                    } else {
                        // Unsigned multiply.
                        uint64_t left_op  = static_cast<uint32_t>(rm_val);
                        uint64_t right_op = static_cast<uint32_t>(rs_val);
                        uint64_t result = left_op * right_op;
                        hi_res = static_cast<int32_t>(result >> 32);
                        lo_res = static_cast<int32_t>(result & 0xffffffff);
                    }
                    set_register(rd_lo, lo_res);
                    set_register(rd_hi, hi_res);
                    if (instr->hasS())
                        MOZ_CRASH();
                }
            } else {
                if (instr->bits(excl::ExclusiveOpHi, excl::ExclusiveOpLo) == excl::ExclusiveOpcode) {
                    // Load-exclusive / store-exclusive.
                    if (instr->bit(excl::ExclusiveLoad)) {
                        int rn = instr->rnValue();
                        int rt = instr->rtValue();
                        int32_t address = get_register(rn);
                        switch (instr->bits(excl::ExclusiveSizeHi, excl::ExclusiveSizeLo)) {
                          case excl::ExclusiveWord:
                            set_register(rt, readExW(address, instr));
                            break;
                          case excl::ExclusiveDouble: {
                            MOZ_ASSERT((rt % 2) == 0);
                            int32_t hibits;
                            int32_t lobits = readExDW(address, &hibits);
                            set_register(rt, lobits);
                            set_register(rt+1, hibits);
                            break;
                          }
                          case excl::ExclusiveByte:
                            set_register(rt, readExBU(address));
                            break;
                          case excl::ExclusiveHalf:
                            set_register(rt, readExHU(address, instr));
                            break;
                        }
                    } else {
                        int rn = instr->rnValue();
                        int rd = instr->rdValue();
                        int rt = instr->bits(3,0);
                        int32_t address = get_register(rn);
                        int32_t value = get_register(rt);
                        int32_t result = 0;
                        switch (instr->bits(excl::ExclusiveSizeHi, excl::ExclusiveSizeLo)) {
                          case excl::ExclusiveWord:
                            result = writeExW(address, value, instr);
                            break;
                          case excl::ExclusiveDouble: {
                            MOZ_ASSERT((rt % 2) == 0);
                            int32_t value2 = get_register(rt+1);
                            result = writeExDW(address, value, value2);
                            break;
                          }
                          case excl::ExclusiveByte:
                            result = writeExB(address, (uint8_t)value);
                            break;
                          case excl::ExclusiveHalf:
                            result = writeExH(address, (uint16_t)value, instr);
                            break;
                        }
                        set_register(rd, result);
                    }
                } else {
                    MOZ_CRASH(); // Not used atm
                }
            }
        } else {
            // Extra load/store instructions.
            int rd = instr->rdValue();
            int rn = instr->rnValue();
            int32_t rn_val = get_register(rn);
            int32_t addr = 0;
            if (instr->bit(22) == 0) {
                int rm = instr->rmValue();
                int32_t rm_val = get_register(rm);
                switch (instr->PUField()) {
                  case da_x:
                    MOZ_ASSERT(!instr->hasW());
                    addr = rn_val;
                    rn_val -= rm_val;
                    set_register(rn, rn_val);
                    break;
                  case ia_x:
                    MOZ_ASSERT(!instr->hasW());
                    addr = rn_val;
                    rn_val += rm_val;
                    set_register(rn, rn_val);
                    break;
                  case db_x:
                    rn_val -= rm_val;
                    addr = rn_val;
                    if (instr->hasW())
                        set_register(rn, rn_val);
                    break;
                  case ib_x:
                    rn_val += rm_val;
                    addr = rn_val;
                    if (instr->hasW())
                        set_register(rn, rn_val);
                    break;
                  default:
                    // The PU field is a 2-bit field.
                    MOZ_CRASH();
                    break;
                }
            } else {
                int32_t imm_val = (instr->immedHValue() << 4) | instr->immedLValue();
                switch (instr->PUField()) {
                  case da_x:
                    MOZ_ASSERT(!instr->hasW());
                    addr = rn_val;
                    rn_val -= imm_val;
                    set_register(rn, rn_val);
                    break;
                  case ia_x:
                    MOZ_ASSERT(!instr->hasW());
                    addr = rn_val;
                    rn_val += imm_val;
                    set_register(rn, rn_val);
                    break;
                  case db_x:
                    rn_val -= imm_val;
                    addr = rn_val;
                    if (instr->hasW())
                        set_register(rn, rn_val);
                    break;
                  case ib_x:
                    rn_val += imm_val;
                    addr = rn_val;
                    if (instr->hasW())
                        set_register(rn, rn_val);
                    break;
                  default:
                    // The PU field is a 2-bit field.
                    MOZ_CRASH();
                    break;
                }
            }
            if ((instr->bits(7, 4) & 0xd) == 0xd && instr->bit(20) == 0) {
                MOZ_ASSERT((rd % 2) == 0);
                if (instr->hasH()) {
                    // The strd instruction.
                    int32_t value1 = get_register(rd);
                    int32_t value2 = get_register(rd+1);
                    writeDW(addr, value1, value2);
                } else {
                    // The ldrd instruction.
                    int* rn_data = readDW(addr);
                    if (rn_data)
                        set_dw_register(rd, rn_data);
                }
            } else if (instr->hasH()) {
                if (instr->hasSign()) {
                    if (instr->hasL()) {
                        int16_t val = readH(addr, instr);
                        set_register(rd, val);
                    } else {
                        int16_t val = get_register(rd);
                        writeH(addr, val, instr);
                    }
                } else {
                    if (instr->hasL()) {
                        uint16_t val = readHU(addr, instr);
                        set_register(rd, val);
                    } else {
                        uint16_t val = get_register(rd);
                        writeH(addr, val, instr);
                    }
                }
            } else {
                // Signed byte loads.
                MOZ_ASSERT(instr->hasSign());
                MOZ_ASSERT(instr->hasL());
                int8_t val = readB(addr);
                set_register(rd, val);
            }
            return;
        }
    } else if ((type == 0) && instr->isMiscType0()) {
        if (instr->bits(7, 4) == 0) {
            if (instr->bit(21) == 0) {
                // mrs
                int rd = instr->rdValue();
                uint32_t flags;
                if (instr->bit(22) == 0) {
                    // CPSR. Note: The Q flag is not yet implemented!
                    flags = (n_flag_ << 31) |
                        (z_flag_ << 30) |
                        (c_flag_ << 29) |
                        (v_flag_ << 28);
                } else {
                    // SPSR
                    MOZ_CRASH();
                }
                set_register(rd, flags);
            } else {
                // msr
                if (instr->bits(27, 23) == 2) {
                    // Register operand. For now we only emit mask 0b1100.
                    int rm = instr->rmValue();
                    mozilla::DebugOnly<uint32_t> mask = instr->bits(19, 16);
                    MOZ_ASSERT(mask == (3 << 2));

                    uint32_t flags = get_register(rm);
                    n_flag_ = (flags >> 31) & 1;
                    z_flag_ = (flags >> 30) & 1;
                    c_flag_ = (flags >> 29) & 1;
                    v_flag_ = (flags >> 28) & 1;
                } else {
                    MOZ_CRASH();
                }
            }
        } else if (instr->bits(22, 21) == 1) {
            int rm = instr->rmValue();
            switch (instr->bits(7, 4)) {
              case 1:   // BX
                set_pc(get_register(rm));
                break;
              case 3: { // BLX
                uint32_t old_pc = get_pc();
                set_pc(get_register(rm));
                set_register(lr, old_pc + SimInstruction::kInstrSize);
                break;
              }
              case 7: { // BKPT
                fprintf(stderr, "Simulator hit BKPT.\n");
                if (getenv("ARM_SIM_DEBUGGER")) {
                    ArmDebugger dbg(this);
                    dbg.debug();
                } else {
                    fprintf(stderr, "Use ARM_SIM_DEBUGGER=1 to enter the builtin debugger.\n");
                    MOZ_CRASH("ARM simulator breakpoint");
                }
                break;
              }
              default:
                MOZ_CRASH();
            }
        } else if (instr->bits(22, 21) == 3) {
            int rm = instr->rmValue();
            int rd = instr->rdValue();
            switch (instr->bits(7, 4)) {
              case 1: { // CLZ
                uint32_t bits = get_register(rm);
                int leading_zeros = 0;
                if (bits == 0)
                    leading_zeros = 32;
                else
                    leading_zeros = mozilla::CountLeadingZeroes32(bits);
                set_register(rd, leading_zeros);
                break;
              }
              default:
                MOZ_CRASH();
                break;
            }
        } else {
            printf("%08x\n", instr->instructionBits());
            MOZ_CRASH();
        }
    } else if ((type == 1) && instr->isNopType1()) {
        // NOP.
    } else if ((type == 1) && instr->isCsdbType1()) {
        // Speculation barrier. (No-op for the simulator)
    } else {
        int rd = instr->rdValue();
        int rn = instr->rnValue();
        int32_t rn_val = get_register(rn);
        int32_t shifter_operand = 0;
        bool shifter_carry_out = 0;
        if (type == 0) {
            shifter_operand = getShiftRm(instr, &shifter_carry_out);
        } else {
            MOZ_ASSERT(instr->typeValue() == 1);
            shifter_operand = getImm(instr, &shifter_carry_out);
        }
        int32_t alu_out;
        switch (instr->opcodeField()) {
          case OpAnd:
            alu_out = rn_val & shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            }
            break;
          case OpEor:
            alu_out = rn_val ^ shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            }
            break;
          case OpSub:
            alu_out = rn_val - shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(!borrowFrom(rn_val, shifter_operand));
                setVFlag(overflowFrom(alu_out, rn_val, shifter_operand, false));
            }
            break;
          case OpRsb:
            alu_out = shifter_operand - rn_val;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(!borrowFrom(shifter_operand, rn_val));
                setVFlag(overflowFrom(alu_out, shifter_operand, rn_val, false));
            }
            break;
          case OpAdd:
            alu_out = rn_val + shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(carryFrom(rn_val, shifter_operand));
                setVFlag(overflowFrom(alu_out, rn_val, shifter_operand, true));
            }
            break;
          case OpAdc:
            alu_out = rn_val + shifter_operand + getCarry();
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(carryFrom(rn_val, shifter_operand, getCarry()));
                setVFlag(overflowFrom(alu_out, rn_val, shifter_operand, true));
            }
            break;
          case OpSbc:
            alu_out = rn_val - shifter_operand - (getCarry() == 0 ? 1 : 0);
            set_register(rd, alu_out);
            if (instr->hasS())
                MOZ_CRASH();
            break;
          case OpRsc:
            alu_out = shifter_operand - rn_val - (getCarry() == 0 ? 1 : 0);
            set_register(rd, alu_out);
            if (instr->hasS())
                MOZ_CRASH();
            break;
          case OpTst:
            if (instr->hasS()) {
                alu_out = rn_val & shifter_operand;
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            } else {
                alu_out = instr->immedMovwMovtValue();
                set_register(rd, alu_out);
            }
            break;
          case OpTeq:
            if (instr->hasS()) {
                alu_out = rn_val ^ shifter_operand;
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            } else {
                // Other instructions matching this pattern are handled in the
                // miscellaneous instructions part above.
                MOZ_CRASH();
            }
            break;
          case OpCmp:
            if (instr->hasS()) {
                alu_out = rn_val - shifter_operand;
                setNZFlags(alu_out);
                setCFlag(!borrowFrom(rn_val, shifter_operand));
                setVFlag(overflowFrom(alu_out, rn_val, shifter_operand, false));
            } else {
                alu_out = (get_register(rd) & 0xffff) |
                    (instr->immedMovwMovtValue() << 16);
                set_register(rd, alu_out);
            }
            break;
          case OpCmn:
            if (instr->hasS()) {
                alu_out = rn_val + shifter_operand;
                setNZFlags(alu_out);
                setCFlag(carryFrom(rn_val, shifter_operand));
                setVFlag(overflowFrom(alu_out, rn_val, shifter_operand, true));
            } else {
                // Other instructions matching this pattern are handled in the
                // miscellaneous instructions part above.
                MOZ_CRASH();
            }
            break;
          case OpOrr:
            alu_out = rn_val | shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            }
            break;
          case OpMov:
            alu_out = shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            }
            break;
          case OpBic:
            alu_out = rn_val & ~shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            }
            break;
          case OpMvn:
            alu_out = ~shifter_operand;
            set_register(rd, alu_out);
            if (instr->hasS()) {
                setNZFlags(alu_out);
                setCFlag(shifter_carry_out);
            }
            break;
          default:
            MOZ_CRASH();
            break;
        }
    }
}

void
Simulator::decodeType2(SimInstruction* instr)
{
    int rd = instr->rdValue();
    int rn = instr->rnValue();
    int32_t rn_val = get_register(rn);
    int32_t im_val = instr->offset12Value();
    int32_t addr = 0;
    switch (instr->PUField()) {
      case da_x:
        MOZ_ASSERT(!instr->hasW());
        addr = rn_val;
        rn_val -= im_val;
        set_register(rn, rn_val);
        break;
      case ia_x:
        MOZ_ASSERT(!instr->hasW());
        addr = rn_val;
        rn_val += im_val;
        set_register(rn, rn_val);
        break;
      case db_x:
        rn_val -= im_val;
        addr = rn_val;
        if (instr->hasW())
            set_register(rn, rn_val);
        break;
      case ib_x:
        rn_val += im_val;
        addr = rn_val;
        if (instr->hasW())
            set_register(rn, rn_val);
        break;
      default:
        MOZ_CRASH();
        break;
    }
    if (instr->hasB()) {
        if (instr->hasL()) {
            uint8_t val = readBU(addr);
            set_register(rd, val);
        } else {
            uint8_t val = get_register(rd);
            writeB(addr, val);
        }
    } else {
        if (instr->hasL())
            set_register(rd, readW(addr, instr, AllowUnaligned));
        else
            writeW(addr, get_register(rd), instr, AllowUnaligned);
    }
}

static uint32_t
rotateBytes(uint32_t val, int32_t rotate)
{
    switch (rotate) {
      default:
        return val;
      case 1:
        return (val >> 8) | (val << 24);
      case 2:
        return (val >> 16) | (val << 16);
      case 3:
        return (val >> 24) | (val << 8);
    }
}

void
Simulator::decodeType3(SimInstruction* instr)
{
    if (MOZ_UNLIKELY(instr->isUDF())) {
        if (handleWasmIllFault())
            return;
        MOZ_CRASH("illegal instruction encountered");
    }

    int rd = instr->rdValue();
    int rn = instr->rnValue();
    int32_t rn_val = get_register(rn);
    bool shifter_carry_out = 0;
    int32_t shifter_operand = getShiftRm(instr, &shifter_carry_out);
    int32_t addr = 0;
    switch (instr->PUField()) {
      case da_x:
        MOZ_ASSERT(!instr->hasW());
        MOZ_CRASH();
        break;
      case ia_x: {
        if (instr->bit(4) == 0) {
            // Memop.
        } else {
            if (instr->bit(5) == 0) {
                switch (instr->bits(22, 21)) {
                  case 0:
                    if (instr->bit(20) == 0) {
                        if (instr->bit(6) == 0) {
                            // Pkhbt.
                            uint32_t rn_val = get_register(rn);
                            uint32_t rm_val = get_register(instr->rmValue());
                            int32_t shift = instr->bits(11, 7);
                            rm_val <<= shift;
                            set_register(rd, (rn_val & 0xFFFF) | (rm_val & 0xFFFF0000U));
                        } else {
                            // Pkhtb.
                            uint32_t rn_val = get_register(rn);
                            int32_t rm_val = get_register(instr->rmValue());
                            int32_t shift = instr->bits(11, 7);
                            if (shift == 0)
                                shift = 32;
                            rm_val >>= shift;
                            set_register(rd, (rn_val & 0xFFFF0000U) | (rm_val & 0xFFFF));
                        }
                    } else {
                        MOZ_CRASH();
                    }
                    break;
                  case 1:
                    MOZ_CRASH();
                    break;
                  case 2:
                    MOZ_CRASH();
                    break;
                  case 3: {
                    // Usat.
                      int32_t sat_pos = instr->bits(20, 16);
                      int32_t sat_val = (1 << sat_pos) - 1;
                      int32_t shift = instr->bits(11, 7);
                      int32_t shift_type = instr->bit(6);
                      int32_t rm_val = get_register(instr->rmValue());
                      if (shift_type == 0) // LSL
                          rm_val <<= shift;
                      else // ASR
                          rm_val >>= shift;

                      // If saturation occurs, the Q flag should be set in the
                      // CPSR. There is no Q flag yet, and no instruction (MRS)
                      // to read the CPSR directly.
                      if (rm_val > sat_val)
                          rm_val = sat_val;
                      else if (rm_val < 0)
                          rm_val = 0;
                      set_register(rd, rm_val);
                      break;
                  }
                }
            } else {
                switch (instr->bits(22, 21)) {
                  case 0:
                    MOZ_CRASH();
                    break;
                  case 1:
                    if (instr->bits(7,4) == 7 && instr->bits(19,16) == 15) {
                        uint32_t rm_val = rotateBytes(get_register(instr->rmValue()),
                                                      instr->bits(11, 10));
                        if (instr->bit(20)) {
                            // Sxth.
                            set_register(rd, (int32_t)(int16_t)(rm_val & 0xFFFF));
                        }
                        else {
                            // Sxtb.
                            set_register(rd, (int32_t)(int8_t)(rm_val & 0xFF));
                        }
                    } else {
                        MOZ_CRASH();
                    }
                    break;
                  case 2:
                    if ((instr->bit(20) == 0) && (instr->bits(9, 6) == 1)) {
                        if (instr->bits(19, 16) == 0xF) {
                            // Uxtb16.
                            uint32_t rm_val = rotateBytes(get_register(instr->rmValue()),
                                                          instr->bits(11, 10));
                            set_register(rd, (rm_val & 0xFF) | (rm_val & 0xFF0000));
                        } else {
                            MOZ_CRASH();
                        }
                    } else {
                        MOZ_CRASH();
                    }
                    break;
                  case 3:
                    if ((instr->bit(20) == 0) && (instr->bits(9, 6) == 1)) {
                        if (instr->bits(19, 16) == 0xF) {
                            // Uxtb.
                            uint32_t rm_val = rotateBytes(get_register(instr->rmValue()),
                                                          instr->bits(11, 10));
                            set_register(rd, (rm_val & 0xFF));
                        } else {
                            // Uxtab.
                            uint32_t rn_val = get_register(rn);
                            uint32_t rm_val = rotateBytes(get_register(instr->rmValue()),
                                                          instr->bits(11, 10));
                            set_register(rd, rn_val + (rm_val & 0xFF));
                        }
                    } else if ((instr->bit(20) == 1) && (instr->bits(9, 6) == 1)) {
                        if (instr->bits(19, 16) == 0xF) {
                            // Uxth.
                            uint32_t rm_val = rotateBytes(get_register(instr->rmValue()),
                                                          instr->bits(11, 10));
                            set_register(rd, (rm_val & 0xFFFF));
                        } else {
                            // Uxtah.
                            uint32_t rn_val = get_register(rn);
                            uint32_t rm_val = rotateBytes(get_register(instr->rmValue()),
                                                          instr->bits(11, 10));
                            set_register(rd, rn_val + (rm_val & 0xFFFF));
                        }
                    } else {
                        MOZ_CRASH();
                    }
                    break;
                }
            }
            return;
        }
        break;
      }
      case db_x: { // sudiv
        if (instr->bit(22) == 0x0 && instr->bit(20) == 0x1 &&
            instr->bits(15,12) == 0x0f && instr->bits(7, 4) == 0x1) {
            if (!instr->hasW()) {
                // sdiv (in V8 notation matching ARM ISA format) rn = rm/rs.
                int rm = instr->rmValue();
                int32_t rm_val = get_register(rm);
                int rs = instr->rsValue();
                int32_t rs_val = get_register(rs);
                int32_t ret_val = 0;
                MOZ_ASSERT(rs_val != 0);
                if ((rm_val == INT32_MIN) && (rs_val == -1))
                    ret_val = INT32_MIN;
                else
                    ret_val = rm_val / rs_val;
                set_register(rn, ret_val);
                return;
            } else {
                // udiv (in V8 notation matching ARM ISA format) rn = rm/rs.
                int rm = instr->rmValue();
                uint32_t rm_val = get_register(rm);
                int rs = instr->rsValue();
                uint32_t rs_val = get_register(rs);
                uint32_t ret_val = 0;
                MOZ_ASSERT(rs_val != 0);
                ret_val = rm_val / rs_val;
                set_register(rn, ret_val);
                return;
            }
        }

        addr = rn_val - shifter_operand;
        if (instr->hasW())
            set_register(rn, addr);
        break;
      }
      case ib_x: {
        if (instr->hasW() && (instr->bits(6, 4) == 0x5)) {
            uint32_t widthminus1 = static_cast<uint32_t>(instr->bits(20, 16));
            uint32_t lsbit = static_cast<uint32_t>(instr->bits(11, 7));
            uint32_t msbit = widthminus1 + lsbit;
            if (msbit <= 31) {
                if (instr->bit(22)) {
                    // ubfx - unsigned bitfield extract.
                    uint32_t rm_val = static_cast<uint32_t>(get_register(instr->rmValue()));
                    uint32_t extr_val = rm_val << (31 - msbit);
                    extr_val = extr_val >> (31 - widthminus1);
                    set_register(instr->rdValue(), extr_val);
                } else {
                    // sbfx - signed bitfield extract.
                    int32_t rm_val = get_register(instr->rmValue());
                    int32_t extr_val = rm_val << (31 - msbit);
                    extr_val = extr_val >> (31 - widthminus1);
                    set_register(instr->rdValue(), extr_val);
                }
            } else {
                MOZ_CRASH();
            }
            return;
        } else if (!instr->hasW() && (instr->bits(6, 4) == 0x1)) {
            uint32_t lsbit = static_cast<uint32_t>(instr->bits(11, 7));
            uint32_t msbit = static_cast<uint32_t>(instr->bits(20, 16));
            if (msbit >= lsbit) {
                // bfc or bfi - bitfield clear/insert.
                uint32_t rd_val =
                    static_cast<uint32_t>(get_register(instr->rdValue()));
                uint32_t bitcount = msbit - lsbit + 1;
                uint32_t mask = (1 << bitcount) - 1;
                rd_val &= ~(mask << lsbit);
                if (instr->rmValue() != 15) {
                    // bfi - bitfield insert.
                    uint32_t rm_val =
                        static_cast<uint32_t>(get_register(instr->rmValue()));
                    rm_val &= mask;
                    rd_val |= rm_val << lsbit;
                }
                set_register(instr->rdValue(), rd_val);
            } else {
                MOZ_CRASH();
            }
            return;
        } else {
            addr = rn_val + shifter_operand;
            if (instr->hasW())
                set_register(rn, addr);
        }
        break;
      }
      default:
        MOZ_CRASH();
        break;
    }
    if (instr->hasB()) {
        if (instr->hasL()) {
            uint8_t byte = readB(addr);
            set_register(rd, byte);
        } else {
            uint8_t byte = get_register(rd);
            writeB(addr, byte);
        }
    } else {
        if (instr->hasL())
            set_register(rd, readW(addr, instr, AllowUnaligned));
        else
            writeW(addr, get_register(rd), instr, AllowUnaligned);
    }
}

void
Simulator::decodeType4(SimInstruction* instr)
{
    // Only allowed to be set in privileged mode.
    MOZ_ASSERT(instr->bit(22) == 0);
    bool load = instr->hasL();
    handleRList(instr, load);
}

void
Simulator::decodeType5(SimInstruction* instr)
{
    int off = instr->sImmed24Value() << 2;
    intptr_t pc_address = get_pc();
    if (instr->hasLink())
        set_register(lr, pc_address + SimInstruction::kInstrSize);
    int pc_reg = get_register(pc);
    set_pc(pc_reg + off);
}

void
Simulator::decodeType6(SimInstruction* instr)
{
    decodeType6CoprocessorIns(instr);
}

void
Simulator::decodeType7(SimInstruction* instr)
{
    if (instr->bit(24) == 1)
        softwareInterrupt(instr);
    else if (instr->bit(4) == 1 && instr->bits(11,9) != 5)
        decodeType7CoprocessorIns(instr);
    else
        decodeTypeVFP(instr);
}

void
Simulator::decodeType7CoprocessorIns(SimInstruction* instr)
{
    if (instr->bit(20) == 0) {
        // MCR, MCR2
        if (instr->coprocessorValue() == 15) {
            int opc1 = instr->bits(23,21);
            int opc2 = instr->bits(7,5);
            int CRn = instr->bits(19,16);
            int CRm = instr->bits(3,0);
            if (opc1 == 0 && opc2 == 4 && CRn == 7 && CRm == 10) {
                // ARMv6 DSB instruction.  We do not use DSB.
                MOZ_CRASH("DSB not implemented");
            } else if (opc1 == 0 && opc2 == 5 && CRn == 7 && CRm == 10) {
                // ARMv6 DMB instruction.
                AtomicOperations::fenceSeqCst();
            }
            else if (opc1 == 0 && opc2 == 4 && CRn == 7 && CRm == 5) {
                // ARMv6 ISB instruction.  We do not use ISB.
                MOZ_CRASH("ISB not implemented");
            }
            else {
                MOZ_CRASH();
            }
        } else {
            MOZ_CRASH();
        }
    } else {
        // MRC, MRC2
        MOZ_CRASH();
    }
}

void
Simulator::decodeTypeVFP(SimInstruction* instr)
{
    MOZ_ASSERT(instr->typeValue() == 7 && instr->bit(24) == 0);
    MOZ_ASSERT(instr->bits(11, 9) == 0x5);

    // Obtain double precision register codes.
    VFPRegPrecision precision = (instr->szValue() == 1) ? kDoublePrecision : kSinglePrecision;
    int vm = instr->VFPMRegValue(precision);
    int vd = instr->VFPDRegValue(precision);
    int vn = instr->VFPNRegValue(precision);

    if (instr->bit(4) == 0) {
        if (instr->opc1Value() == 0x7) {
            // Other data processing instructions.
            if ((instr->opc2Value() == 0x0) && (instr->opc3Value() == 0x1)) {
                // vmov register to register.
                if (instr->szValue() == 0x1) {
                    int m = instr->VFPMRegValue(kDoublePrecision);
                    int d = instr->VFPDRegValue(kDoublePrecision);
                    double temp;
                    get_double_from_d_register(m, &temp);
                    set_d_register_from_double(d, temp);
                } else {
                    int m = instr->VFPMRegValue(kSinglePrecision);
                    int d = instr->VFPDRegValue(kSinglePrecision);
                    float temp;
                    get_float_from_s_register(m, &temp);
                    set_s_register_from_float(d, temp);
                }
            } else if ((instr->opc2Value() == 0x0) && (instr->opc3Value() == 0x3)) {
                // vabs
                if (instr->szValue() == 0x1) {
                    union {
                        double f64;
                        uint64_t u64;
                    } u;
                    get_double_from_d_register(vm, &u.f64);
                    u.u64 &= 0x7fffffffffffffffu;
                    double dd_value = u.f64;
                    canonicalizeNaN(&dd_value);
                    set_d_register_from_double(vd, dd_value);
                } else {
                    union {
                        float f32;
                        uint32_t u32;
                    } u;
                    get_float_from_s_register(vm, &u.f32);
                    u.u32 &= 0x7fffffffu;
                    float fd_value = u.f32;
                    canonicalizeNaN(&fd_value);
                    set_s_register_from_float(vd, fd_value);
                }
            } else if ((instr->opc2Value() == 0x1) && (instr->opc3Value() == 0x1)) {
                // vneg
                if (instr->szValue() == 0x1) {
                    double dm_value;
                    get_double_from_d_register(vm, &dm_value);
                    double dd_value = -dm_value;
                    canonicalizeNaN(&dd_value);
                    set_d_register_from_double(vd, dd_value);
                } else {
                    float fm_value;
                    get_float_from_s_register(vm, &fm_value);
                    float fd_value = -fm_value;
                    canonicalizeNaN(&fd_value);
                    set_s_register_from_float(vd, fd_value);
                }
            } else if ((instr->opc2Value() == 0x7) && (instr->opc3Value() == 0x3)) {
                decodeVCVTBetweenDoubleAndSingle(instr);
            } else if ((instr->opc2Value() == 0x8) && (instr->opc3Value() & 0x1)) {
                decodeVCVTBetweenFloatingPointAndInteger(instr);
            } else if ((instr->opc2Value() == 0xA) && (instr->opc3Value() == 0x3) &&
                       (instr->bit(8) == 1)) {
                // vcvt.f64.s32 Dd, Dd, #<fbits>.
                int fraction_bits = 32 - ((instr->bits(3, 0) << 1) | instr->bit(5));
                int fixed_value = get_sinteger_from_s_register(vd * 2);
                double divide = 1 << fraction_bits;
                set_d_register_from_double(vd, fixed_value / divide);
            } else if (((instr->opc2Value() >> 1) == 0x6) &&
                       (instr->opc3Value() & 0x1)) {
                decodeVCVTBetweenFloatingPointAndInteger(instr);
            } else if (((instr->opc2Value() == 0x4) || (instr->opc2Value() == 0x5)) &&
                       (instr->opc3Value() & 0x1)) {
                decodeVCMP(instr);
            } else if (((instr->opc2Value() == 0x1)) && (instr->opc3Value() == 0x3)) {
                // vsqrt
                if (instr->szValue() == 0x1) {
                    double dm_value;
                    get_double_from_d_register(vm, &dm_value);
                    double dd_value = std::sqrt(dm_value);
                    canonicalizeNaN(&dd_value);
                    set_d_register_from_double(vd, dd_value);
                } else {
                    float fm_value;
                    get_float_from_s_register(vm, &fm_value);
                    float fd_value = std::sqrt(fm_value);
                    canonicalizeNaN(&fd_value);
                    set_s_register_from_float(vd, fd_value);
                }
            } else if (instr->opc3Value() == 0x0) {
                // vmov immediate.
                if (instr->szValue() == 0x1) {
                    set_d_register_from_double(vd, instr->doubleImmedVmov());
                } else {
                    // vmov.f32 immediate.
                    set_s_register_from_float(vd, instr->float32ImmedVmov());
                }
            } else {
                decodeVCVTBetweenFloatingPointAndIntegerFrac(instr);
            }
        } else if (instr->opc1Value() == 0x3) {
            if (instr->szValue() != 0x1) {
                if (instr->opc3Value() & 0x1) {
                    // vsub
                    float fn_value;
                    get_float_from_s_register(vn, &fn_value);
                    float fm_value;
                    get_float_from_s_register(vm, &fm_value);
                    float fd_value = fn_value - fm_value;
                    canonicalizeNaN(&fd_value);
                    set_s_register_from_float(vd, fd_value);
                } else {
                    // vadd
                    float fn_value;
                    get_float_from_s_register(vn, &fn_value);
                    float fm_value;
                    get_float_from_s_register(vm, &fm_value);
                    float fd_value = fn_value + fm_value;
                    canonicalizeNaN(&fd_value);
                    set_s_register_from_float(vd, fd_value);
                }
            } else {
                if (instr->opc3Value() & 0x1) {
                    // vsub
                    double dn_value;
                    get_double_from_d_register(vn, &dn_value);
                    double dm_value;
                    get_double_from_d_register(vm, &dm_value);
                    double dd_value = dn_value - dm_value;
                    canonicalizeNaN(&dd_value);
                    set_d_register_from_double(vd, dd_value);
                } else {
                    // vadd
                    double dn_value;
                    get_double_from_d_register(vn, &dn_value);
                    double dm_value;
                    get_double_from_d_register(vm, &dm_value);
                    double dd_value = dn_value + dm_value;
                    canonicalizeNaN(&dd_value);
                    set_d_register_from_double(vd, dd_value);
                }
            }
        } else if ((instr->opc1Value() == 0x2) && !(instr->opc3Value() & 0x1)) {
            // vmul
            if (instr->szValue() != 0x1) {
                float fn_value;
                get_float_from_s_register(vn, &fn_value);
                float fm_value;
                get_float_from_s_register(vm, &fm_value);
                float fd_value = fn_value * fm_value;
                canonicalizeNaN(&fd_value);
                set_s_register_from_float(vd, fd_value);
            } else {
                double dn_value;
                get_double_from_d_register(vn, &dn_value);
                double dm_value;
                get_double_from_d_register(vm, &dm_value);
                double dd_value = dn_value * dm_value;
                canonicalizeNaN(&dd_value);
                set_d_register_from_double(vd, dd_value);
            }
        } else if ((instr->opc1Value() == 0x0)) {
            // vmla, vmls
            const bool is_vmls = (instr->opc3Value() & 0x1);

            if (instr->szValue() != 0x1)
                MOZ_CRASH("Not used by V8.");

            double dd_val;
            get_double_from_d_register(vd, &dd_val);
            double dn_val;
            get_double_from_d_register(vn, &dn_val);
            double dm_val;
            get_double_from_d_register(vm, &dm_val);

            // Note: we do the mul and add/sub in separate steps to avoid
            // getting a result with too high precision.
            set_d_register_from_double(vd, dn_val * dm_val);
            double temp;
            get_double_from_d_register(vd, &temp);
            if (is_vmls)
                temp = dd_val - temp;
            else
                temp = dd_val + temp;
            canonicalizeNaN(&temp);
            set_d_register_from_double(vd, temp);
        } else if ((instr->opc1Value() == 0x4) && !(instr->opc3Value() & 0x1)) {
            // vdiv
            if (instr->szValue() != 0x1) {
                float fn_value;
                get_float_from_s_register(vn, &fn_value);
                float fm_value;
                get_float_from_s_register(vm, &fm_value);
                float fd_value = fn_value / fm_value;
                div_zero_vfp_flag_ = (fm_value == 0);
                canonicalizeNaN(&fd_value);
                set_s_register_from_float(vd, fd_value);
            } else {
                double dn_value;
                get_double_from_d_register(vn, &dn_value);
                double dm_value;
                get_double_from_d_register(vm, &dm_value);
                double dd_value = dn_value / dm_value;
                div_zero_vfp_flag_ = (dm_value == 0);
                canonicalizeNaN(&dd_value);
                set_d_register_from_double(vd, dd_value);
            }
        } else {
            MOZ_CRASH();
        }
    } else {
        if (instr->VCValue() == 0x0 && instr->VAValue() == 0x0) {
            decodeVMOVBetweenCoreAndSinglePrecisionRegisters(instr);
        } else if ((instr->VLValue() == 0x0) &&
                   (instr->VCValue() == 0x1) &&
                   (instr->bit(23) == 0x0)) {
            // vmov (ARM core register to scalar).
            int vd = instr->bits(19, 16) | (instr->bit(7) << 4);
            double dd_value;
            get_double_from_d_register(vd, &dd_value);
            int32_t data[2];
            memcpy(data, &dd_value, 8);
            data[instr->bit(21)] = get_register(instr->rtValue());
            memcpy(&dd_value, data, 8);
            set_d_register_from_double(vd, dd_value);
        } else if ((instr->VLValue() == 0x1) &&
                   (instr->VCValue() == 0x1) &&
                   (instr->bit(23) == 0x0)) {
            // vmov (scalar to ARM core register).
            int vn = instr->bits(19, 16) | (instr->bit(7) << 4);
            double dn_value;
            get_double_from_d_register(vn, &dn_value);
            int32_t data[2];
            memcpy(data, &dn_value, 8);
            set_register(instr->rtValue(), data[instr->bit(21)]);
        } else if ((instr->VLValue() == 0x1) &&
                   (instr->VCValue() == 0x0) &&
                   (instr->VAValue() == 0x7) &&
                   (instr->bits(19, 16) == 0x1)) {
            // vmrs
            uint32_t rt = instr->rtValue();
            if (rt == 0xF) {
                copy_FPSCR_to_APSR();
            } else {
                // Emulate FPSCR from the Simulator flags.
                uint32_t fpscr = (n_flag_FPSCR_ << 31) |
                    (z_flag_FPSCR_ << 30) |
                    (c_flag_FPSCR_ << 29) |
                    (v_flag_FPSCR_ << 28) |
                    (FPSCR_default_NaN_mode_ << 25) |
                    (inexact_vfp_flag_ << 4) |
                    (underflow_vfp_flag_ << 3) |
                    (overflow_vfp_flag_ << 2) |
                    (div_zero_vfp_flag_ << 1) |
                    (inv_op_vfp_flag_ << 0) |
                    (FPSCR_rounding_mode_);
                set_register(rt, fpscr);
            }
        } else if ((instr->VLValue() == 0x0) &&
                   (instr->VCValue() == 0x0) &&
                   (instr->VAValue() == 0x7) &&
                   (instr->bits(19, 16) == 0x1)) {
            // vmsr
            uint32_t rt = instr->rtValue();
            if (rt == pc) {
                MOZ_CRASH();
            } else {
                uint32_t rt_value = get_register(rt);
                n_flag_FPSCR_ = (rt_value >> 31) & 1;
                z_flag_FPSCR_ = (rt_value >> 30) & 1;
                c_flag_FPSCR_ = (rt_value >> 29) & 1;
                v_flag_FPSCR_ = (rt_value >> 28) & 1;
                FPSCR_default_NaN_mode_ = (rt_value >> 25) & 1;
                inexact_vfp_flag_ = (rt_value >> 4) & 1;
                underflow_vfp_flag_ = (rt_value >> 3) & 1;
                overflow_vfp_flag_ = (rt_value >> 2) & 1;
                div_zero_vfp_flag_ = (rt_value >> 1) & 1;
                inv_op_vfp_flag_ = (rt_value >> 0) & 1;
                FPSCR_rounding_mode_ =
                    static_cast<VFPRoundingMode>((rt_value) & kVFPRoundingModeMask);
            }
        } else {
            MOZ_CRASH();
        }
    }
}

void
Simulator::decodeVMOVBetweenCoreAndSinglePrecisionRegisters(SimInstruction* instr)
{
    MOZ_ASSERT(instr->bit(4) == 1 &&
               instr->VCValue() == 0x0 &&
               instr->VAValue() == 0x0);

    int t = instr->rtValue();
    int n = instr->VFPNRegValue(kSinglePrecision);
    bool to_arm_register = (instr->VLValue() == 0x1);
    if (to_arm_register) {
        int32_t int_value = get_sinteger_from_s_register(n);
        set_register(t, int_value);
    } else {
        int32_t rs_val = get_register(t);
        set_s_register_from_sinteger(n, rs_val);
    }
}

void
Simulator::decodeVCMP(SimInstruction* instr)
{
    MOZ_ASSERT((instr->bit(4) == 0) && (instr->opc1Value() == 0x7));
    MOZ_ASSERT(((instr->opc2Value() == 0x4) || (instr->opc2Value() == 0x5)) &&
               (instr->opc3Value() & 0x1));
    // Comparison.

    VFPRegPrecision precision = kSinglePrecision;
    if (instr->szValue() == 1)
        precision = kDoublePrecision;

    int d = instr->VFPDRegValue(precision);
    int m = 0;
    if (instr->opc2Value() == 0x4)
        m = instr->VFPMRegValue(precision);

    if (precision == kDoublePrecision) {
        double dd_value;
        get_double_from_d_register(d, &dd_value);
        double dm_value = 0.0;
        if (instr->opc2Value() == 0x4)
            get_double_from_d_register(m, &dm_value);

        // Raise exceptions for quiet NaNs if necessary.
        if (instr->bit(7) == 1) {
            if (mozilla::IsNaN(dd_value))
                inv_op_vfp_flag_ = true;
        }
        compute_FPSCR_Flags(dd_value, dm_value);
    } else {
        float fd_value;
        get_float_from_s_register(d, &fd_value);
        float fm_value = 0.0;
        if (instr->opc2Value() == 0x4)
            get_float_from_s_register(m, &fm_value);

        // Raise exceptions for quiet NaNs if necessary.
        if (instr->bit(7) == 1) {
            if (mozilla::IsNaN(fd_value))
                inv_op_vfp_flag_ = true;
        }
        compute_FPSCR_Flags(fd_value, fm_value);
    }
}

void
Simulator::decodeVCVTBetweenDoubleAndSingle(SimInstruction* instr)
{
    MOZ_ASSERT(instr->bit(4) == 0 && instr->opc1Value() == 0x7);
    MOZ_ASSERT(instr->opc2Value() == 0x7 && instr->opc3Value() == 0x3);

    VFPRegPrecision dst_precision = kDoublePrecision;
    VFPRegPrecision src_precision = kSinglePrecision;
    if (instr->szValue() == 1) {
        dst_precision = kSinglePrecision;
        src_precision = kDoublePrecision;
    }

    int dst = instr->VFPDRegValue(dst_precision);
    int src = instr->VFPMRegValue(src_precision);

    if (dst_precision == kSinglePrecision) {
        double val;
        get_double_from_d_register(src, &val);
        set_s_register_from_float(dst, static_cast<float>(val));
    } else {
        float val;
        get_float_from_s_register(src, &val);
        set_d_register_from_double(dst, static_cast<double>(val));
    }
}

static bool
get_inv_op_vfp_flag(VFPRoundingMode mode, double val, bool unsigned_)
{
    MOZ_ASSERT(mode == SimRN || mode == SimRM || mode == SimRZ);
    double max_uint = static_cast<double>(0xffffffffu);
    double max_int = static_cast<double>(INT32_MAX);
    double min_int = static_cast<double>(INT32_MIN);

    // Check for NaN.
    if (val != val)
        return true;

    // Check for overflow. This code works because 32bit integers can be exactly
    // represented by ieee-754 64bit floating-point values.
    switch (mode) {
      case SimRN:
        return  unsigned_ ? (val >= (max_uint + 0.5)) ||
                            (val < -0.5)
                          : (val >= (max_int + 0.5)) ||
                            (val < (min_int - 0.5));
      case SimRM:
        return  unsigned_ ? (val >= (max_uint + 1.0)) ||
                            (val < 0)
                          : (val >= (max_int + 1.0)) ||
                            (val < min_int);
      case SimRZ:
        return  unsigned_ ? (val >= (max_uint + 1.0)) ||
                            (val <= -1)
                          : (val >= (max_int + 1.0)) ||
                            (val <= (min_int - 1.0));
      default:
        MOZ_CRASH();
        return true;
    }
}

// We call this function only if we had a vfp invalid exception.
// It returns the correct saturated value.
static int
VFPConversionSaturate(double val, bool unsigned_res)
{
    if (val != val) // NaN.
        return 0;
    if (unsigned_res)
        return (val < 0) ? 0 : 0xffffffffu;
    return (val < 0) ? INT32_MIN : INT32_MAX;
}

void
Simulator::decodeVCVTBetweenFloatingPointAndInteger(SimInstruction* instr)
{
    MOZ_ASSERT((instr->bit(4) == 0) && (instr->opc1Value() == 0x7) &&
               (instr->bits(27, 23) == 0x1D));
    MOZ_ASSERT(((instr->opc2Value() == 0x8) && (instr->opc3Value() & 0x1)) ||
               (((instr->opc2Value() >> 1) == 0x6) && (instr->opc3Value() & 0x1)));

    // Conversion between floating-point and integer.
    bool to_integer = (instr->bit(18) == 1);

    VFPRegPrecision src_precision = (instr->szValue() == 1) ? kDoublePrecision : kSinglePrecision;

    if (to_integer) {
        // We are playing with code close to the C++ standard's limits below,
        // hence the very simple code and heavy checks.
        //
        // Note: C++ defines default type casting from floating point to integer
        // as (close to) rounding toward zero ("fractional part discarded").

        int dst = instr->VFPDRegValue(kSinglePrecision);
        int src = instr->VFPMRegValue(src_precision);

        // Bit 7 in vcvt instructions indicates if we should use the FPSCR
        // rounding mode or the default Round to Zero mode.
        VFPRoundingMode mode = (instr->bit(7) != 1) ? FPSCR_rounding_mode_ : SimRZ;
        MOZ_ASSERT(mode == SimRM || mode == SimRZ || mode == SimRN);

        bool unsigned_integer = (instr->bit(16) == 0);
        bool double_precision = (src_precision == kDoublePrecision);

        double val;
        if (double_precision) {
            get_double_from_d_register(src, &val);
        } else {
            float fval;
            get_float_from_s_register(src, &fval);
            val = double(fval);
        }

        int temp = unsigned_integer ? static_cast<uint32_t>(val) : static_cast<int32_t>(val);

        inv_op_vfp_flag_ = get_inv_op_vfp_flag(mode, val, unsigned_integer);

        double abs_diff = unsigned_integer
                          ? std::fabs(val - static_cast<uint32_t>(temp))
                          : std::fabs(val - temp);

        inexact_vfp_flag_ = (abs_diff != 0);

        if (inv_op_vfp_flag_) {
            temp = VFPConversionSaturate(val, unsigned_integer);
        } else {
            switch (mode) {
              case SimRN: {
                int val_sign = (val > 0) ? 1 : -1;
                if (abs_diff > 0.5) {
                    temp += val_sign;
                } else if (abs_diff == 0.5) {
                    // Round to even if exactly halfway.
                    temp = ((temp % 2) == 0) ? temp : temp + val_sign;
                }
                break;
              }

              case SimRM:
                temp = temp > val ? temp - 1 : temp;
                  break;

              case SimRZ:
                // Nothing to do.
                break;

              default:
                MOZ_CRASH();
            }
        }

        // Update the destination register.
        set_s_register_from_sinteger(dst, temp);
    } else {
        bool unsigned_integer = (instr->bit(7) == 0);
        int dst = instr->VFPDRegValue(src_precision);
        int src = instr->VFPMRegValue(kSinglePrecision);

        int val = get_sinteger_from_s_register(src);

        if (src_precision == kDoublePrecision) {
            if (unsigned_integer)
                set_d_register_from_double(dst, static_cast<double>(static_cast<uint32_t>(val)));
            else
                set_d_register_from_double(dst, static_cast<double>(val));
        } else {
            if (unsigned_integer)
                set_s_register_from_float(dst, static_cast<float>(static_cast<uint32_t>(val)));
            else
                set_s_register_from_float(dst, static_cast<float>(val));
        }
    }
}

// A VFPv3 specific instruction.
void
Simulator::decodeVCVTBetweenFloatingPointAndIntegerFrac(SimInstruction* instr)
{
    MOZ_ASSERT(instr->bits(27, 24) == 0xE && instr->opc1Value() == 0x7 && instr->bit(19) == 1 &&
               instr->bit(17) == 1 && instr->bits(11,9) == 0x5 && instr->bit(6) == 1 &&
               instr->bit(4) == 0);

    int size = (instr->bit(7) == 1) ? 32 : 16;

    int fraction_bits = size - ((instr->bits(3, 0) << 1) | instr->bit(5));
    double mult = 1 << fraction_bits;

    MOZ_ASSERT(size == 32); // Only handling size == 32 for now.

    // Conversion between floating-point and integer.
    bool to_fixed = (instr->bit(18) == 1);

    VFPRegPrecision precision = (instr->szValue() == 1) ? kDoublePrecision : kSinglePrecision;

    if (to_fixed) {
        // We are playing with code close to the C++ standard's limits below,
        // hence the very simple code and heavy checks.
        //
        // Note: C++ defines default type casting from floating point to integer
        // as (close to) rounding toward zero ("fractional part discarded").

        int dst = instr->VFPDRegValue(precision);

        bool unsigned_integer = (instr->bit(16) == 1);
        bool double_precision = (precision == kDoublePrecision);

        double val;
        if (double_precision) {
            get_double_from_d_register(dst, &val);
        } else {
            float fval;
            get_float_from_s_register(dst, &fval);
            val = double(fval);
        }

        // Scale value by specified number of fraction bits.
        val *= mult;

        // Rounding down towards zero. No need to account for the rounding error
        // as this instruction always rounds down towards zero. See SimRZ below.
        int temp = unsigned_integer ? static_cast<uint32_t>(val) : static_cast<int32_t>(val);

        inv_op_vfp_flag_ = get_inv_op_vfp_flag(SimRZ, val, unsigned_integer);

        double abs_diff = unsigned_integer
                          ? std::fabs(val - static_cast<uint32_t>(temp))
                          : std::fabs(val - temp);

        inexact_vfp_flag_ = (abs_diff != 0);

        if (inv_op_vfp_flag_)
            temp = VFPConversionSaturate(val, unsigned_integer);

        // Update the destination register.
        if (double_precision) {
            uint32_t dbl[2];
            dbl[0] = temp; dbl[1] = 0;
            set_d_register(dst, dbl);
        } else {
            set_s_register_from_sinteger(dst, temp);
        }
    } else {
        MOZ_CRASH();  // Not implemented, fixed to float.
    }
}

void
Simulator::decodeType6CoprocessorIns(SimInstruction* instr)
{
    MOZ_ASSERT(instr->typeValue() == 6);

    if (instr->coprocessorValue() == 0xA) {
        switch (instr->opcodeValue()) {
          case 0x8:
          case 0xA:
          case 0xC:
          case 0xE: {  // Load and store single precision float to memory.
            int rn = instr->rnValue();
            int vd = instr->VFPDRegValue(kSinglePrecision);
            int offset = instr->immed8Value();
            if (!instr->hasU())
                offset = -offset;

            int32_t address = get_register(rn) + 4 * offset;
            if (instr->hasL()) {
                // Load double from memory: vldr.
                set_s_register_from_sinteger(vd, readW(address, instr));
            } else {
                // Store double to memory: vstr.
                writeW(address, get_sinteger_from_s_register(vd), instr);
            }
            break;
          }
          case 0x4:
          case 0x5:
          case 0x6:
          case 0x7:
          case 0x9:
          case 0xB:
            // Load/store multiple single from memory: vldm/vstm.
            handleVList(instr);
            break;
          default:
            MOZ_CRASH();
        }
    } else if (instr->coprocessorValue() == 0xB) {
        switch (instr->opcodeValue()) {
          case 0x2:
            // Load and store double to two GP registers
            if (instr->bits(7, 6) != 0 || instr->bit(4) != 1) {
                MOZ_CRASH();  // Not used atm.
            } else {
                int rt = instr->rtValue();
                int rn = instr->rnValue();
                int vm = instr->VFPMRegValue(kDoublePrecision);
                if (instr->hasL()) {
                    int32_t data[2];
                    double d;
                    get_double_from_d_register(vm, &d);
                    memcpy(data, &d, 8);
                    set_register(rt, data[0]);
                    set_register(rn, data[1]);
                } else {
                    int32_t data[] = { get_register(rt), get_register(rn) };
                    double d;
                    memcpy(&d, data, 8);
                    set_d_register_from_double(vm, d);
                }
            }
            break;
          case 0x8:
          case 0xA:
          case 0xC:
          case 0xE: {  // Load and store double to memory.
            int rn = instr->rnValue();
            int vd = instr->VFPDRegValue(kDoublePrecision);
            int offset = instr->immed8Value();
            if (!instr->hasU())
                offset = -offset;
            int32_t address = get_register(rn) + 4 * offset;
            if (instr->hasL()) {
                // Load double from memory: vldr.
                uint64_t data = readQ(address, instr);
                double val;
                memcpy(&val, &data, 8);
                set_d_register_from_double(vd, val);
            } else {
                // Store double to memory: vstr.
                uint64_t data;
                double val;
                get_double_from_d_register(vd, &val);
                memcpy(&data, &val, 8);
                writeQ(address, data, instr);
            }
            break;
          }
          case 0x4:
          case 0x5:
          case 0x6:
          case 0x7:
          case 0x9:
          case 0xB:
            // Load/store multiple double from memory: vldm/vstm.
            handleVList(instr);
            break;
          default:
            MOZ_CRASH();
        }
    } else {
        MOZ_CRASH();
    }
}

void
Simulator::decodeSpecialCondition(SimInstruction* instr)
{
    switch (instr->specialValue()) {
      case 5:
        if (instr->bits(18, 16) == 0 && instr->bits(11, 6) == 0x28 && instr->bit(4) == 1) {
            // vmovl signed
            if ((instr->vdValue() & 1) != 0)
                MOZ_CRASH("Undefined behavior");
            int Vd = (instr->bit(22) << 3) | (instr->vdValue() >> 1);
            int Vm = (instr->bit(5) << 4) | instr->vmValue();
            int imm3 = instr->bits(21, 19);
            if (imm3 != 1 && imm3 != 2 && imm3 != 4)
                MOZ_CRASH();
            int esize = 8 * imm3;
            int elements = 64 / esize;
            int8_t from[8];
            get_d_register(Vm, reinterpret_cast<uint64_t*>(from));
            int16_t to[8];
            int e = 0;
            while (e < elements) {
                to[e] = from[e];
                e++;
            }
            set_q_register(Vd, reinterpret_cast<uint64_t*>(to));
        } else {
            MOZ_CRASH();
        }
        break;
      case 7:
        if (instr->bits(18, 16) == 0 && instr->bits(11, 6) == 0x28 && instr->bit(4) == 1) {
            // vmovl unsigned.
            if ((instr->vdValue() & 1) != 0)
                MOZ_CRASH("Undefined behavior");
            int Vd = (instr->bit(22) << 3) | (instr->vdValue() >> 1);
            int Vm = (instr->bit(5) << 4) | instr->vmValue();
            int imm3 = instr->bits(21, 19);
            if (imm3 != 1 && imm3 != 2 && imm3 != 4)
                MOZ_CRASH();
            int esize = 8 * imm3;
            int elements = 64 / esize;
            uint8_t from[8];
            get_d_register(Vm, reinterpret_cast<uint64_t*>(from));
            uint16_t to[8];
            int e = 0;
            while (e < elements) {
                to[e] = from[e];
                e++;
            }
            set_q_register(Vd, reinterpret_cast<uint64_t*>(to));
        } else {
            MOZ_CRASH();
        }
        break;
      case 8:
        if (instr->bits(21, 20) == 0) {
            // vst1
            int Vd = (instr->bit(22) << 4) | instr->vdValue();
            int Rn = instr->vnValue();
            int type = instr->bits(11, 8);
            int Rm = instr->vmValue();
            int32_t address = get_register(Rn);
            int regs = 0;
            switch (type) {
              case nlt_1:
                regs = 1;
                break;
              case nlt_2:
                regs = 2;
                break;
              case nlt_3:
                regs = 3;
                break;
              case nlt_4:
                regs = 4;
                break;
              default:
                MOZ_CRASH();
                break;
            }
            int r = 0;
            while (r < regs) {
                uint32_t data[2];
                get_d_register(Vd + r, data);
                // TODO: We should AllowUnaligned here only if the alignment attribute of
                // the instruction calls for default alignment.
                writeW(address, data[0], instr, AllowUnaligned);
                writeW(address + 4, data[1], instr, AllowUnaligned);
                address += 8;
                r++;
            }
            if (Rm != 15) {
                if (Rm == 13)
                    set_register(Rn, address);
                else
                    set_register(Rn, get_register(Rn) + get_register(Rm));
            }
        } else if (instr->bits(21, 20) == 2) {
            // vld1
            int Vd = (instr->bit(22) << 4) | instr->vdValue();
            int Rn = instr->vnValue();
            int type = instr->bits(11, 8);
            int Rm = instr->vmValue();
            int32_t address = get_register(Rn);
            int regs = 0;
            switch (type) {
              case nlt_1:
                regs = 1;
                break;
              case nlt_2:
                regs = 2;
                break;
              case nlt_3:
                regs = 3;
                break;
              case nlt_4:
                regs = 4;
                break;
              default:
                MOZ_CRASH();
                break;
            }
            int r = 0;
            while (r < regs) {
                uint32_t data[2];
                // TODO: We should AllowUnaligned here only if the alignment attribute of
                // the instruction calls for default alignment.
                data[0] = readW(address, instr, AllowUnaligned);
                data[1] = readW(address + 4, instr, AllowUnaligned);
                set_d_register(Vd + r, data);
                address += 8;
                r++;
            }
            if (Rm != 15) {
                if (Rm == 13)
                    set_register(Rn, address);
                else
                    set_register(Rn, get_register(Rn) + get_register(Rm));
            }
        } else {
            MOZ_CRASH();
        }
        break;
      case 0xA:
        if (instr->bits(31,20) == 0xf57) {
            switch (instr->bits(7,4)) {
              case 1: // CLREX
                exclusiveMonitorClear();
                break;
              case 5: // DMB
                AtomicOperations::fenceSeqCst();
                break;
              case 4: // DSB
                // We do not use DSB.
                MOZ_CRASH("DSB unimplemented");
              case 6: // ISB
                // We do not use ISB.
                MOZ_CRASH("ISB unimplemented");
              default:
                MOZ_CRASH();
            }
        } else {
            MOZ_CRASH();
        }
        break;
      case 0xB:
        if (instr->bits(22, 20) == 5 && instr->bits(15, 12) == 0xf) {
            // pld: ignore instruction.
        } else {
            MOZ_CRASH();
        }
        break;
      case 0x1C:
      case 0x1D:
        if (instr->bit(4) == 1 && instr->bits(11,9) != 5) {
            // MCR, MCR2, MRC, MRC2 with cond == 15
            decodeType7CoprocessorIns(instr);
        } else {
            MOZ_CRASH();
        }
        break;
      default:
        MOZ_CRASH();
    }
}

// Executes the current instruction.
void
Simulator::instructionDecode(SimInstruction* instr)
{
    if (!SimulatorProcess::ICacheCheckingDisableCount) {
        AutoLockSimulatorCache als;
        SimulatorProcess::checkICacheLocked(instr);
    }

    pc_modified_ = false;

    static const uint32_t kSpecialCondition = 15 << 28;
    if (instr->conditionField() == kSpecialCondition) {
        decodeSpecialCondition(instr);
    } else if (conditionallyExecute(instr)) {
        switch (instr->typeValue()) {
          case 0:
          case 1:
            decodeType01(instr);
            break;
          case 2:
            decodeType2(instr);
            break;
          case 3:
            decodeType3(instr);
            break;
          case 4:
            decodeType4(instr);
            break;
          case 5:
            decodeType5(instr);
            break;
          case 6:
            decodeType6(instr);
            break;
          case 7:
            decodeType7(instr);
            break;
          default:
            MOZ_CRASH();
            break;
        }
        // If the instruction is a non taken conditional stop, we need to skip
        // the inlined message address.
    } else if (instr->isStop()) {
        set_pc(get_pc() + 2 * SimInstruction::kInstrSize);
    }
    if (!pc_modified_)
        set_register(pc, reinterpret_cast<int32_t>(instr) + SimInstruction::kInstrSize);
}

void
Simulator::enable_single_stepping(SingleStepCallback cb, void* arg)
{
    single_stepping_ = true;
    single_step_callback_ = cb;
    single_step_callback_arg_ = arg;
    single_step_callback_(single_step_callback_arg_, this, (void*)get_pc());
}

void
Simulator::disable_single_stepping()
{
    if (!single_stepping_)
        return;
    single_step_callback_(single_step_callback_arg_, this, (void*)get_pc());
    single_stepping_ = false;
    single_step_callback_ = nullptr;
    single_step_callback_arg_ = nullptr;
}

static void
FakeInterruptHandler()
{
    JSContext* cx = TlsContext.get();
    uint8_t* pc = cx->simulator()->get_pc_as<uint8_t*>();

    const wasm::ModuleSegment* ms= nullptr;
    if (!wasm::InInterruptibleCode(cx, pc, &ms))
        return;

    cx->simulator()->trigger_wasm_interrupt();
}

template<bool EnableStopSimAt>
void
Simulator::execute()
{
    if (single_stepping_)
        single_step_callback_(single_step_callback_arg_, this, nullptr);

    // Get the PC to simulate. Cannot use the accessor here as we need the raw
    // PC value and not the one used as input to arithmetic instructions.
    int program_counter = get_pc();

    while (program_counter != end_sim_pc) {
        if (EnableStopSimAt && (icount_ == Simulator::StopSimAt)) {
            fprintf(stderr, "\nStopped simulation at icount %lld\n", icount_);
            ArmDebugger dbg(this);
            dbg.debug();
        } else {
            if (single_stepping_)
                single_step_callback_(single_step_callback_arg_, this, (void*)program_counter);
            if (MOZ_UNLIKELY(JitOptions.simulatorAlwaysInterrupt))
                FakeInterruptHandler();
            SimInstruction* instr = reinterpret_cast<SimInstruction*>(program_counter);
            instructionDecode(instr);
            icount_++;

            if (MOZ_UNLIKELY(wasm_interrupt_)) {
                handleWasmInterrupt();
                wasm_interrupt_ = false;
            }
        }
        program_counter = get_pc();
    }

    if (single_stepping_)
        single_step_callback_(single_step_callback_arg_, this, nullptr);
}

void
Simulator::callInternal(uint8_t* entry)
{
    // Prepare to execute the code at entry.
    set_register(pc, reinterpret_cast<int32_t>(entry));

    // Put down marker for end of simulation. The simulator will stop simulation
    // when the PC reaches this value. By saving the "end simulation" value into
    // the LR the simulation stops when returning to this call point.
    set_register(lr, end_sim_pc);

    // Remember the values of callee-saved registers. The code below assumes
    // that r9 is not used as sb (static base) in simulator code and therefore
    // is regarded as a callee-saved register.
    int32_t r4_val = get_register(r4);
    int32_t r5_val = get_register(r5);
    int32_t r6_val = get_register(r6);
    int32_t r7_val = get_register(r7);
    int32_t r8_val = get_register(r8);
    int32_t r9_val = get_register(r9);
    int32_t r10_val = get_register(r10);
    int32_t r11_val = get_register(r11);

    // Remember d8 to d15 which are callee-saved.
    uint64_t d8_val;
    get_d_register(d8, &d8_val);
    uint64_t d9_val;
    get_d_register(d9, &d9_val);
    uint64_t d10_val;
    get_d_register(d10, &d10_val);
    uint64_t d11_val;
    get_d_register(d11, &d11_val);
    uint64_t d12_val;
    get_d_register(d12, &d12_val);
    uint64_t d13_val;
    get_d_register(d13, &d13_val);
    uint64_t d14_val;
    get_d_register(d14, &d14_val);
    uint64_t d15_val;
    get_d_register(d15, &d15_val);

    // Set up the callee-saved registers with a known value. To be able to check
    // that they are preserved properly across JS execution.
    int32_t callee_saved_value = uint32_t(icount_);
    uint64_t callee_saved_value_d = uint64_t(icount_);

    if (!skipCalleeSavedRegsCheck) {
        set_register(r4, callee_saved_value);
        set_register(r5, callee_saved_value);
        set_register(r6, callee_saved_value);
        set_register(r7, callee_saved_value);
        set_register(r8, callee_saved_value);
        set_register(r9, callee_saved_value);
        set_register(r10, callee_saved_value);
        set_register(r11, callee_saved_value);

        set_d_register(d8, &callee_saved_value_d);
        set_d_register(d9, &callee_saved_value_d);
        set_d_register(d10, &callee_saved_value_d);
        set_d_register(d11, &callee_saved_value_d);
        set_d_register(d12, &callee_saved_value_d);
        set_d_register(d13, &callee_saved_value_d);
        set_d_register(d14, &callee_saved_value_d);
        set_d_register(d15, &callee_saved_value_d);

    }
    // Start the simulation.
    if (Simulator::StopSimAt != -1L)
        execute<true>();
    else
        execute<false>();

    if (!skipCalleeSavedRegsCheck) {
        // Check that the callee-saved registers have been preserved.
        MOZ_ASSERT(callee_saved_value == get_register(r4));
        MOZ_ASSERT(callee_saved_value == get_register(r5));
        MOZ_ASSERT(callee_saved_value == get_register(r6));
        MOZ_ASSERT(callee_saved_value == get_register(r7));
        MOZ_ASSERT(callee_saved_value == get_register(r8));
        MOZ_ASSERT(callee_saved_value == get_register(r9));
        MOZ_ASSERT(callee_saved_value == get_register(r10));
        MOZ_ASSERT(callee_saved_value == get_register(r11));

        uint64_t value;
        get_d_register(d8, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d9, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d10, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d11, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d12, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d13, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d14, &value);
        MOZ_ASSERT(callee_saved_value_d == value);
        get_d_register(d15, &value);
        MOZ_ASSERT(callee_saved_value_d == value);

        // Restore callee-saved registers with the original value.
        set_register(r4, r4_val);
        set_register(r5, r5_val);
        set_register(r6, r6_val);
        set_register(r7, r7_val);
        set_register(r8, r8_val);
        set_register(r9, r9_val);
        set_register(r10, r10_val);
        set_register(r11, r11_val);

        set_d_register(d8, &d8_val);
        set_d_register(d9, &d9_val);
        set_d_register(d10, &d10_val);
        set_d_register(d11, &d11_val);
        set_d_register(d12, &d12_val);
        set_d_register(d13, &d13_val);
        set_d_register(d14, &d14_val);
        set_d_register(d15, &d15_val);
    }
}

int32_t
Simulator::call(uint8_t* entry, int argument_count, ...)
{
    va_list parameters;
    va_start(parameters, argument_count);

    // First four arguments passed in registers.
    MOZ_ASSERT(argument_count >= 1);
    set_register(r0, va_arg(parameters, int32_t));
    if (argument_count >= 2)
        set_register(r1, va_arg(parameters, int32_t));
    if (argument_count >= 3)
        set_register(r2, va_arg(parameters, int32_t));
    if (argument_count >= 4)
        set_register(r3, va_arg(parameters, int32_t));

    // Remaining arguments passed on stack.
    int original_stack = get_register(sp);
    int entry_stack = original_stack;
    if (argument_count >= 4)
        entry_stack -= (argument_count - 4) * sizeof(int32_t);

    entry_stack &= ~ABIStackAlignment;

    // Store remaining arguments on stack, from low to high memory.
    intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack);
    for (int i = 4; i < argument_count; i++)
        stack_argument[i - 4] = va_arg(parameters, int32_t);
    va_end(parameters);
    set_register(sp, entry_stack);

    callInternal(entry);

    // Pop stack passed arguments.
    MOZ_ASSERT(entry_stack == get_register(sp));
    set_register(sp, original_stack);

    int32_t result = get_register(r0);
    return result;
}

Simulator*
Simulator::Current()
{
    JSContext* cx = TlsContext.get();
    MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
    return cx->simulator();
}

} // namespace jit
} // namespace js

js::jit::Simulator*
JSContext::simulator() const
{
    return simulator_;
}

uintptr_t*
JSContext::addressOfSimulatorStackLimit()
{
    return simulator_->addressOfStackLimit();
}
