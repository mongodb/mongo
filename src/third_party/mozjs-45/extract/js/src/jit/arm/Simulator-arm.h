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

#ifndef jit_arm_Simulator_arm_h
#define jit_arm_Simulator_arm_h

#ifdef JS_SIMULATOR_ARM

#include "jslock.h"

#include "jit/arm/Architecture-arm.h"
#include "jit/arm/disasm/Disasm-arm.h"
#include "jit/IonTypes.h"

namespace js {
namespace jit {

class Simulator;
class Redirection;
class CachePage;
class AutoLockSimulator;

// When the SingleStepCallback is called, the simulator is about to execute
// sim->get_pc() and the current machine state represents the completed
// execution of the previous pc.
typedef void (*SingleStepCallback)(void* arg, Simulator* sim, void* pc);

// VFP rounding modes. See ARM DDI 0406B Page A2-29.
enum VFPRoundingMode {
    SimRN = 0 << 22,   // Round to Nearest.
    SimRP = 1 << 22,   // Round towards Plus Infinity.
    SimRM = 2 << 22,   // Round towards Minus Infinity.
    SimRZ = 3 << 22,   // Round towards zero.

    // Aliases.
    kRoundToNearest = SimRN,
    kRoundToPlusInf = SimRP,
    kRoundToMinusInf = SimRM,
    kRoundToZero = SimRZ
};

const uint32_t kVFPRoundingModeMask = 3 << 22;

typedef int32_t Instr;
class SimInstruction;

class Simulator
{
    friend class Redirection;
    friend class AutoLockSimulatorCache;

  public:
    friend class ArmDebugger;
    enum Register {
        no_reg = -1,
        r0 = 0, r1, r2, r3, r4, r5, r6, r7,
        r8, r9, r10, r11, r12, r13, r14, r15,
        num_registers,
        sp = 13,
        lr = 14,
        pc = 15,
        s0 = 0, s1, s2, s3, s4, s5, s6, s7,
        s8, s9, s10, s11, s12, s13, s14, s15,
        s16, s17, s18, s19, s20, s21, s22, s23,
        s24, s25, s26, s27, s28, s29, s30, s31,
        num_s_registers = 32,
        d0 = 0, d1, d2, d3, d4, d5, d6, d7,
        d8, d9, d10, d11, d12, d13, d14, d15,
        d16, d17, d18, d19, d20, d21, d22, d23,
        d24, d25, d26, d27, d28, d29, d30, d31,
        num_d_registers = 32,
        q0 = 0, q1, q2, q3, q4, q5, q6, q7,
        q8, q9, q10, q11, q12, q13, q14, q15,
        num_q_registers = 16
    };

    // Returns nullptr on OOM.
    static Simulator* Create();

    static void Destroy(Simulator* simulator);

    // Constructor/destructor are for internal use only; use the static methods above.
    Simulator();
    ~Simulator();

    // The currently executing Simulator instance. Potentially there can be one
    // for each native thread.
    static Simulator* Current();

    static inline uintptr_t StackLimit() {
        return Simulator::Current()->stackLimit();
    }

    uintptr_t* addressOfStackLimit();

    // Accessors for register state. Reading the pc value adheres to the ARM
    // architecture specification and is off by a 8 from the currently executing
    // instruction.
    void set_register(int reg, int32_t value);
    int32_t get_register(int reg) const;
    double get_double_from_register_pair(int reg);
    void set_register_pair_from_double(int reg, double* value);
    void set_dw_register(int dreg, const int* dbl);

    // Support for VFP.
    void get_d_register(int dreg, uint64_t* value);
    void set_d_register(int dreg, const uint64_t* value);
    void get_d_register(int dreg, uint32_t* value);
    void set_d_register(int dreg, const uint32_t* value);
    void get_q_register(int qreg, uint64_t* value);
    void set_q_register(int qreg, const uint64_t* value);
    void get_q_register(int qreg, uint32_t* value);
    void set_q_register(int qreg, const uint32_t* value);
    void set_s_register(int reg, unsigned int value);
    unsigned int get_s_register(int reg) const;

    void set_d_register_from_double(int dreg, const double& dbl) {
        setVFPRegister<double, 2>(dreg, dbl);
    }
    double get_double_from_d_register(int dreg) {
        return getFromVFPRegister<double, 2>(dreg);
    }
    void set_s_register_from_float(int sreg, const float flt) {
        setVFPRegister<float, 1>(sreg, flt);
    }
    float get_float_from_s_register(int sreg) {
        return getFromVFPRegister<float, 1>(sreg);
    }
    void set_s_register_from_sinteger(int sreg, const int sint) {
        setVFPRegister<int, 1>(sreg, sint);
    }
    int get_sinteger_from_s_register(int sreg) {
        return getFromVFPRegister<int, 1>(sreg);
    }

    // Special case of set_register and get_register to access the raw PC value.
    void set_pc(int32_t value);
    int32_t get_pc() const;

    template <typename T>
    T get_pc_as() const { return reinterpret_cast<T>(get_pc()); }

    void set_resume_pc(void* value) {
        resume_pc_ = int32_t(value);
    }

    void enable_single_stepping(SingleStepCallback cb, void* arg);
    void disable_single_stepping();

    uintptr_t stackLimit() const;
    bool overRecursed(uintptr_t newsp = 0) const;
    bool overRecursedWithExtra(uint32_t extra) const;

    // Executes ARM instructions until the PC reaches end_sim_pc.
    template<bool EnableStopSimAt>
    void execute();

    // Sets up the simulator state and grabs the result on return.
    int32_t call(uint8_t* entry, int argument_count, ...);

    // Debugger input.
    void setLastDebuggerInput(char* input);
    char* lastDebuggerInput() { return lastDebuggerInput_; }

    // Returns true if pc register contains one of the 'special_values' defined
    // below (bad_lr, end_sim_pc).
    bool has_bad_pc() const;

  private:
    enum special_values {
        // Known bad pc value to ensure that the simulator does not execute
        // without being properly setup.
        bad_lr = -1,
        // A pc value used to signal the simulator to stop execution. Generally
        // the lr is set to this value on transition from native C code to
        // simulated execution, so that the simulator can "return" to the native
        // C code.
        end_sim_pc = -2
    };

    bool init();

    // Checks if the current instruction should be executed based on its
    // condition bits.
    inline bool conditionallyExecute(SimInstruction* instr);

    // Helper functions to set the conditional flags in the architecture state.
    void setNZFlags(int32_t val);
    void setCFlag(bool val);
    void setVFlag(bool val);
    bool carryFrom(int32_t left, int32_t right, int32_t carry = 0);
    bool borrowFrom(int32_t left, int32_t right);
    bool overflowFrom(int32_t alu_out, int32_t left, int32_t right, bool addition);

    inline int getCarry() { return c_flag_ ? 1 : 0; };

    // Support for VFP.
    void compute_FPSCR_Flags(double val1, double val2);
    void copy_FPSCR_to_APSR();
    inline double canonicalizeNaN(double value);

    // Helper functions to decode common "addressing" modes
    int32_t getShiftRm(SimInstruction* instr, bool* carry_out);
    int32_t getImm(SimInstruction* instr, bool* carry_out);
    int32_t processPU(SimInstruction* instr, int num_regs, int operand_size,
                      intptr_t* start_address, intptr_t* end_address);
    void handleRList(SimInstruction* instr, bool load);
    void handleVList(SimInstruction* inst);
    void softwareInterrupt(SimInstruction* instr);

    // Stop helper functions.
    inline bool isStopInstruction(SimInstruction* instr);
    inline bool isWatchedStop(uint32_t bkpt_code);
    inline bool isEnabledStop(uint32_t bkpt_code);
    inline void enableStop(uint32_t bkpt_code);
    inline void disableStop(uint32_t bkpt_code);
    inline void increaseStopCounter(uint32_t bkpt_code);
    void printStopInfo(uint32_t code);

    // Read and write memory.
    inline uint8_t readBU(int32_t addr);
    inline int8_t readB(int32_t addr);
    inline void writeB(int32_t addr, uint8_t value);
    inline void writeB(int32_t addr, int8_t value);

    inline uint8_t readExBU(int32_t addr);
    inline int32_t writeExB(int32_t addr, uint8_t value);

    inline uint16_t readHU(int32_t addr, SimInstruction* instr);
    inline int16_t readH(int32_t addr, SimInstruction* instr);
    // Note: Overloaded on the sign of the value.
    inline void writeH(int32_t addr, uint16_t value, SimInstruction* instr);
    inline void writeH(int32_t addr, int16_t value, SimInstruction* instr);

    inline uint16_t readExHU(int32_t addr, SimInstruction* instr);
    inline int32_t writeExH(int32_t addr, uint16_t value, SimInstruction* instr);

    inline int readW(int32_t addr, SimInstruction* instr);
    inline void writeW(int32_t addr, int value, SimInstruction* instr);

    inline int readExW(int32_t addr, SimInstruction* instr);
    inline int writeExW(int32_t addr, int value, SimInstruction* instr);

    int32_t* readDW(int32_t addr);
    void writeDW(int32_t addr, int32_t value1, int32_t value2);

    int32_t readExDW(int32_t addr, int32_t* hibits);
    int32_t writeExDW(int32_t addr, int32_t value1, int32_t value2);

    // Executing is handled based on the instruction type.
    // Both type 0 and type 1 rolled into one.
    void decodeType01(SimInstruction* instr);
    void decodeType2(SimInstruction* instr);
    void decodeType3(SimInstruction* instr);
    void decodeType4(SimInstruction* instr);
    void decodeType5(SimInstruction* instr);
    void decodeType6(SimInstruction* instr);
    void decodeType7(SimInstruction* instr);

    // Support for VFP.
    void decodeTypeVFP(SimInstruction* instr);
    void decodeType6CoprocessorIns(SimInstruction* instr);
    void decodeSpecialCondition(SimInstruction* instr);

    void decodeVMOVBetweenCoreAndSinglePrecisionRegisters(SimInstruction* instr);
    void decodeVCMP(SimInstruction* instr);
    void decodeVCVTBetweenDoubleAndSingle(SimInstruction* instr);
    void decodeVCVTBetweenFloatingPointAndInteger(SimInstruction* instr);
    void decodeVCVTBetweenFloatingPointAndIntegerFrac(SimInstruction* instr);

    // Support for some system functions.
    void decodeType7CoprocessorIns(SimInstruction* instr);

    // Executes one instruction.
    void instructionDecode(SimInstruction* instr);

  public:
    static bool ICacheCheckingEnabled;
    static void FlushICache(void* start, size_t size);

    static int64_t StopSimAt;

    // For testing the MoveResolver code, a MoveResolver is set up, and
    // the VFP registers are loaded with pre-determined values,
    // then the sequence of code is simulated.  In order to test this with the
    // simulator, the callee-saved registers can't be trashed. This flag
    // disables that feature.
    bool skipCalleeSavedRegsCheck;

    // Runtime call support.
    static void* RedirectNativeFunction(void* nativeFunction, ABIFunctionType type);

  private:
    // Handle arguments and return value for runtime FP functions.
    void getFpArgs(double* x, double* y, int32_t* z);
    void getFpFromStack(int32_t* stack, double* x1);
    void setCallResultDouble(double result);
    void setCallResultFloat(float result);
    void setCallResult(int64_t res);
    void scratchVolatileRegisters(bool scratchFloat = true);

    template<class ReturnType, int register_size>
    ReturnType getFromVFPRegister(int reg_index);

    template<class InputType, int register_size>
    void setVFPRegister(int reg_index, const InputType& value);

    void callInternal(uint8_t* entry);

    // Architecture state.
    // Saturating instructions require a Q flag to indicate saturation.
    // There is currently no way to read the CPSR directly, and thus read the Q
    // flag, so this is left unimplemented.
    int32_t registers_[16];
    bool n_flag_;
    bool z_flag_;
    bool c_flag_;
    bool v_flag_;

    // VFP architecture state.
    uint32_t vfp_registers_[num_d_registers * 2];
    bool n_flag_FPSCR_;
    bool z_flag_FPSCR_;
    bool c_flag_FPSCR_;
    bool v_flag_FPSCR_;

    // VFP rounding mode. See ARM DDI 0406B Page A2-29.
    VFPRoundingMode FPSCR_rounding_mode_;
    bool FPSCR_default_NaN_mode_;

    // VFP FP exception flags architecture state.
    bool inv_op_vfp_flag_;
    bool div_zero_vfp_flag_;
    bool overflow_vfp_flag_;
    bool underflow_vfp_flag_;
    bool inexact_vfp_flag_;

    // Simulator support.
    char* stack_;
    uintptr_t stackLimit_;
    bool pc_modified_;
    int64_t icount_;

    int32_t resume_pc_;

    // Debugger input.
    char* lastDebuggerInput_;

    // Registered breakpoints.
    SimInstruction* break_pc_;
    Instr break_instr_;

    // Single-stepping support
    bool single_stepping_;
    SingleStepCallback single_step_callback_;
    void* single_step_callback_arg_;

    // A stop is watched if its code is less than kNumOfWatchedStops.
    // Only watched stops support enabling/disabling and the counter feature.
    static const uint32_t kNumOfWatchedStops = 256;

    // Breakpoint is disabled if bit 31 is set.
    static const uint32_t kStopDisabledBit = 1 << 31;

    // A stop is enabled, meaning the simulator will stop when meeting the
    // instruction, if bit 31 of watched_stops_[code].count is unset.
    // The value watched_stops_[code].count & ~(1 << 31) indicates how many times
    // the breakpoint was hit or gone through.
    struct StopCountAndDesc {
        uint32_t count;
        char* desc;
    };
    StopCountAndDesc watched_stops_[kNumOfWatchedStops];

  public:
    int64_t icount() {
        return icount_;
    }

  private:
    // ICache checking.
    struct ICacheHasher {
        typedef void* Key;
        typedef void* Lookup;
        static HashNumber hash(const Lookup& l);
        static bool match(const Key& k, const Lookup& l);
    };

  public:
    typedef HashMap<void*, CachePage*, ICacheHasher, SystemAllocPolicy> ICacheMap;

  private:
    // This lock creates a critical section around 'redirection_' and
    // 'icache_', which are referenced both by the execution engine
    // and by the off-thread compiler (see Redirection::Get in the cpp file).
    PRLock* cacheLock_;
#ifdef DEBUG
    PRThread* cacheLockHolder_;
#endif

    Redirection* redirection_;
    ICacheMap icache_;

  public:
    ICacheMap& icache() {
        // Technically we need the lock to access the innards of the
        // icache, not to take its address, but the latter condition
        // serves as a useful complement to the former.
        MOZ_ASSERT(cacheLockHolder_);
        return icache_;
    }

    Redirection* redirection() const {
        MOZ_ASSERT(cacheLockHolder_);
        return redirection_;
    }

    void setRedirection(js::jit::Redirection* redirection) {
        MOZ_ASSERT(cacheLockHolder_);
        redirection_ = redirection;
    }

  private:
    // Exclusive access monitor
    void exclusiveMonitorSet(uint64_t value);
    uint64_t exclusiveMonitorGetAndClear(bool* held);
    void exclusiveMonitorClear();

    bool exclusiveMonitorHeld_;
    uint64_t exclusiveMonitor_;
};

#define JS_CHECK_SIMULATOR_RECURSION_WITH_EXTRA(cx, extra, onerror)             \
    JS_BEGIN_MACRO                                                              \
        if (cx->runtime()->simulator()->overRecursedWithExtra(extra)) {         \
            js::ReportOverRecursed(cx);                                         \
            onerror;                                                            \
        }                                                                       \
    JS_END_MACRO

} // namespace jit
} // namespace js

#endif /* JS_SIMULATOR_ARM */

#endif /* jit_arm_Simulator_arm_h */
