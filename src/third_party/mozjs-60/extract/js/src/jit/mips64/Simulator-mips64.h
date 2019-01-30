/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */
// Copyright 2011 the V8 project authors. All rights reserved.
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

#ifndef jit_mips64_Simulator_mips64_h
#define jit_mips64_Simulator_mips64_h

#ifdef JS_SIMULATOR_MIPS64

#include "mozilla/Atomics.h"

#include "jit/IonTypes.h"
#include "js/ProfilingFrameIterator.h"
#include "threading/Thread.h"
#include "vm/MutexIDs.h"

namespace js {

namespace jit {

class JitActivation;

class Simulator;
class Redirection;
class CachePage;
class AutoLockSimulator;

// When the SingleStepCallback is called, the simulator is about to execute
// sim->get_pc() and the current machine state represents the completed
// execution of the previous pc.
typedef void (*SingleStepCallback)(void* arg, Simulator* sim, void* pc);

const intptr_t kPointerAlignment = 8;
const intptr_t kPointerAlignmentMask = kPointerAlignment - 1;

const intptr_t kDoubleAlignment = 8;
const intptr_t kDoubleAlignmentMask = kDoubleAlignment - 1;


// Number of general purpose registers.
const int kNumRegisters = 32;

// In the simulator, the PC register is simulated as the 34th register.
const int kPCRegister = 34;

// Number coprocessor registers.
const int kNumFPURegisters = 32;

// FPU (coprocessor 1) control registers. Currently only FCSR is implemented.
const int kFCSRRegister = 31;
const int kInvalidFPUControlRegister = -1;
const uint32_t kFPUInvalidResult = static_cast<uint32_t>(1 << 31) - 1;

// FCSR constants.
const uint32_t kFCSRInexactFlagBit = 2;
const uint32_t kFCSRUnderflowFlagBit = 3;
const uint32_t kFCSROverflowFlagBit = 4;
const uint32_t kFCSRDivideByZeroFlagBit = 5;
const uint32_t kFCSRInvalidOpFlagBit = 6;

const uint32_t kFCSRInexactCauseBit = 12;
const uint32_t kFCSRUnderflowCauseBit = 13;
const uint32_t kFCSROverflowCauseBit = 14;
const uint32_t kFCSRDivideByZeroCauseBit = 15;
const uint32_t kFCSRInvalidOpCauseBit = 16;

const uint32_t kFCSRInexactFlagMask = 1 << kFCSRInexactFlagBit;
const uint32_t kFCSRUnderflowFlagMask = 1 << kFCSRUnderflowFlagBit;
const uint32_t kFCSROverflowFlagMask = 1 << kFCSROverflowFlagBit;
const uint32_t kFCSRDivideByZeroFlagMask = 1 << kFCSRDivideByZeroFlagBit;
const uint32_t kFCSRInvalidOpFlagMask = 1 << kFCSRInvalidOpFlagBit;

const uint32_t kFCSRFlagMask =
    kFCSRInexactFlagMask |
    kFCSRUnderflowFlagMask |
    kFCSROverflowFlagMask |
    kFCSRDivideByZeroFlagMask |
    kFCSRInvalidOpFlagMask;

const uint32_t kFCSRExceptionFlagMask = kFCSRFlagMask ^ kFCSRInexactFlagMask;

// On MIPS64 Simulator breakpoints can have different codes:
// - Breaks between 0 and kMaxWatchpointCode are treated as simple watchpoints,
//   the simulator will run through them and print the registers.
// - Breaks between kMaxWatchpointCode and kMaxStopCode are treated as stop()
//   instructions (see Assembler::stop()).
// - Breaks larger than kMaxStopCode are simple breaks, dropping you into the
//   debugger.
const uint32_t kMaxWatchpointCode = 31;
const uint32_t kMaxStopCode = 127;
const uint32_t kWasmTrapCode = 6;

// -----------------------------------------------------------------------------
// Utility functions

typedef uint32_t Instr;
class SimInstruction;

// Per thread simulator state.
class Simulator {
    friend class MipsDebugger;
  public:

    // Registers are declared in order. See "See MIPS Run Linux" chapter 2.
    enum Register {
        no_reg = -1,
        zero_reg = 0,
        at,
        v0, v1,
        a0, a1, a2, a3, a4, a5, a6, a7,
        t0, t1, t2, t3,
        s0, s1, s2, s3, s4, s5, s6, s7,
        t8, t9,
        k0, k1,
        gp,
        sp,
        s8,
        ra,
        // LO, HI, and pc.
        LO,
        HI,
        pc,   // pc must be the last register.
        kNumSimuRegisters,
        // aliases
        fp = s8
    };

    // Coprocessor registers.
    enum FPURegister {
        f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11,
        f12, f13, f14, f15, f16, f17, f18, f19, f20, f21,
        f22, f23, f24, f25, f26, f27, f28, f29, f30, f31,
        kNumFPURegisters
    };

    // Returns nullptr on OOM.
    static Simulator* Create(JSContext* cx);

    static void Destroy(Simulator* simulator);

    // Constructor/destructor are for internal use only; use the static methods above.
    Simulator();
    ~Simulator();

    static bool supportsAtomics() { return true; }

    // The currently executing Simulator instance. Potentially there can be one
    // for each native thread.
    static Simulator* Current();

    static inline uintptr_t StackLimit() {
        return Simulator::Current()->stackLimit();
    }

    uintptr_t* addressOfStackLimit();

    // Accessors for register state. Reading the pc value adheres to the MIPS
    // architecture specification and is off by a 8 from the currently executing
    // instruction.
    void setRegister(int reg, int64_t value);
    int64_t getRegister(int reg) const;
    // Same for FPURegisters.
    void setFpuRegister(int fpureg, int64_t value);
    void setFpuRegisterLo(int fpureg, int32_t value);
    void setFpuRegisterHi(int fpureg, int32_t value);
    void setFpuRegisterFloat(int fpureg, float value);
    void setFpuRegisterDouble(int fpureg, double value);
    int64_t getFpuRegister(int fpureg) const;
    int32_t getFpuRegisterLo(int fpureg) const;
    int32_t getFpuRegisterHi(int fpureg) const;
    float getFpuRegisterFloat(int fpureg) const;
    double getFpuRegisterDouble(int fpureg) const;
    void setFCSRBit(uint32_t cc, bool value);
    bool testFCSRBit(uint32_t cc);
    bool setFCSRRoundError(double original, double rounded);

    // Special case of set_register and get_register to access the raw PC value.
    void set_pc(int64_t value);
    int64_t get_pc() const;

    template <typename T>
    T get_pc_as() const { return reinterpret_cast<T>(get_pc()); }

    void trigger_wasm_interrupt() {
        // This can be called several times if a single interrupt isn't caught
        // and handled by the simulator, but this is fine; once the current
        // instruction is done executing, the interrupt will be handled anyhow.
        wasm_interrupt_ = true;
    }

    void enable_single_stepping(SingleStepCallback cb, void* arg);
    void disable_single_stepping();

    // Accessor to the internal simulator stack area.
    uintptr_t stackLimit() const;
    bool overRecursed(uintptr_t newsp = 0) const;
    bool overRecursedWithExtra(uint32_t extra) const;

    // Executes MIPS instructions until the PC reaches end_sim_pc.
    template<bool enableStopSimAt>
    void execute();

    // Sets up the simulator state and grabs the result on return.
    int64_t call(uint8_t* entry, int argument_count, ...);

    // Push an address onto the JS stack.
    uintptr_t pushAddress(uintptr_t address);

    // Pop an address from the JS stack.
    uintptr_t popAddress();

    // Debugger input.
    void setLastDebuggerInput(char* input);
    char* lastDebuggerInput() { return lastDebuggerInput_; }

    // Returns true if pc register contains one of the 'SpecialValues' defined
    // below (bad_ra, end_sim_pc).
    bool has_bad_pc() const;

  private:
    enum SpecialValues {
        // Known bad pc value to ensure that the simulator does not execute
        // without being properly setup.
        bad_ra = -1,
        // A pc value used to signal the simulator to stop execution.  Generally
        // the ra is set to this value on transition from native C code to
        // simulated execution, so that the simulator can "return" to the native
        // C code.
        end_sim_pc = -2,
        // Unpredictable value.
        Unpredictable = 0xbadbeaf
    };

    bool init();

    // Unsupported instructions use Format to print an error and stop execution.
    void format(SimInstruction* instr, const char* format);

    // Read and write memory.
    inline uint8_t readBU(uint64_t addr, SimInstruction* instr);
    inline int8_t readB(uint64_t addr, SimInstruction* instr);
    inline void writeB(uint64_t addr, uint8_t value, SimInstruction* instr);
    inline void writeB(uint64_t addr, int8_t value, SimInstruction* instr);

    inline uint16_t readHU(uint64_t addr, SimInstruction* instr);
    inline int16_t readH(uint64_t addr, SimInstruction* instr);
    inline void writeH(uint64_t addr, uint16_t value, SimInstruction* instr);
    inline void writeH(uint64_t addr, int16_t value, SimInstruction* instr);

    inline uint32_t readWU(uint64_t addr, SimInstruction* instr);
    inline int32_t readW(uint64_t addr, SimInstruction* instr);
    inline void writeW(uint64_t addr, uint32_t value, SimInstruction* instr);
    inline void writeW(uint64_t addr, int32_t value, SimInstruction* instr);

    inline int64_t readDW(uint64_t addr, SimInstruction* instr);
    inline int64_t readDWL(uint64_t addr, SimInstruction* instr);
    inline int64_t readDWR(uint64_t addr, SimInstruction* instr);
    inline void writeDW(uint64_t addr, int64_t value, SimInstruction* instr);

    inline double readD(uint64_t addr, SimInstruction* instr);
    inline void writeD(uint64_t addr, double value, SimInstruction* instr);

    inline int32_t loadLinkedW(uint64_t addr, SimInstruction* instr);
    inline int storeConditionalW(uint64_t addr, int32_t value, SimInstruction* instr);

    inline int64_t loadLinkedD(uint64_t addr, SimInstruction* instr);
    inline int storeConditionalD(uint64_t addr, int64_t value, SimInstruction* instr);

    // Helper function for decodeTypeRegister.
    void configureTypeRegister(SimInstruction* instr,
                               int64_t& alu_out,
                               __int128& i128hilo,
                               unsigned __int128& u128hilo,
                               int64_t& next_pc,
                               int32_t& return_addr_reg,
                               bool& do_interrupt);

    // Executing is handled based on the instruction type.
    void decodeTypeRegister(SimInstruction* instr);
    void decodeTypeImmediate(SimInstruction* instr);
    void decodeTypeJump(SimInstruction* instr);

    // Used for breakpoints and traps.
    void softwareInterrupt(SimInstruction* instr);

    // Stop helper functions.
    bool isWatchpoint(uint32_t code);
    void printWatchpoint(uint32_t code);
    void handleStop(uint32_t code, SimInstruction* instr);
    bool isStopInstruction(SimInstruction* instr);
    bool isEnabledStop(uint32_t code);
    void enableStop(uint32_t code);
    void disableStop(uint32_t code);
    void increaseStopCounter(uint32_t code);
    void printStopInfo(uint32_t code);

    // Handle a wasm interrupt triggered by an async signal handler.
    void handleWasmInterrupt();
    JS::ProfilingFrameIterator::RegisterState registerState();

    // Handle any wasm faults, returning true if the fault was handled.
    bool handleWasmFault(uint64_t addr, unsigned numBytes);
    bool handleWasmTrapFault();

    // Executes one instruction.
    void instructionDecode(SimInstruction* instr);
    // Execute one instruction placed in a branch delay slot.
    void branchDelayInstructionDecode(SimInstruction* instr);

  public:
    static int64_t StopSimAt;

    // Runtime call support.
    static void* RedirectNativeFunction(void* nativeFunction, ABIFunctionType type);

  private:
    enum Exception {
        kNone,
        kIntegerOverflow,
        kIntegerUnderflow,
        kDivideByZero,
        kNumExceptions
    };
    int16_t exceptions[kNumExceptions];

    // Exceptions.
    void signalExceptions();

    // Handle return value for runtime FP functions.
    void setCallResultDouble(double result);
    void setCallResultFloat(float result);
    void setCallResult(int64_t res);
    void setCallResult(__int128 res);

    void callInternal(uint8_t* entry);

    // Architecture state.
    // Registers.
    int64_t registers_[kNumSimuRegisters];
    // Coprocessor Registers.
    int64_t FPUregisters_[kNumFPURegisters];
    // FPU control register.
    uint32_t FCSR_;

    bool LLBit_;
    uintptr_t LLAddr_;
    int64_t lastLLValue_;

    // Simulator support.
    char* stack_;
    uintptr_t stackLimit_;
    bool pc_modified_;
    int64_t icount_;
    int64_t break_count_;

    // wasm async interrupt support
    bool wasm_interrupt_;

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


    // Stop is disabled if bit 31 is set.
    static const uint32_t kStopDisabledBit = 1U << 31;

    // A stop is enabled, meaning the simulator will stop when meeting the
    // instruction, if bit 31 of watchedStops_[code].count is unset.
    // The value watchedStops_[code].count & ~(1 << 31) indicates how many times
    // the breakpoint was hit or gone through.
    struct StopCountAndDesc {
        uint32_t count_;
        char* desc_;
    };
    StopCountAndDesc watchedStops_[kNumOfWatchedStops];
};

// Process wide simulator state.
class SimulatorProcess
{
    friend class Redirection;
    friend class AutoLockSimulatorCache;

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

    static mozilla::Atomic<size_t, mozilla::ReleaseAcquire> ICacheCheckingDisableCount;
    static void FlushICache(void* start, size_t size);

    // Jitcode may be rewritten from a signal handler, but is prevented from
    // calling FlushICache() because the signal may arrive within the critical
    // area of an AutoLockSimulatorCache. This flag instructs the Simulator
    // to remove all cache entries the next time it checks, avoiding false negatives.
    static mozilla::Atomic<bool, mozilla::ReleaseAcquire> cacheInvalidatedBySignalHandler_;

    static void checkICacheLocked(SimInstruction* instr);

    static bool initialize() {
        singleton_ = js_new<SimulatorProcess>();
        return singleton_ && singleton_->init();
    }
    static void destroy() {
        js_delete(singleton_);
        singleton_ = nullptr;
    }

    SimulatorProcess();
    ~SimulatorProcess();

  private:
    bool init();

    static SimulatorProcess* singleton_;

    // This lock creates a critical section around 'redirection_' and
    // 'icache_', which are referenced both by the execution engine
    // and by the off-thread compiler (see Redirection::Get in the cpp file).
    Mutex cacheLock_;

    Redirection* redirection_;
    ICacheMap icache_;

  public:
    static ICacheMap& icache() {
        // Technically we need the lock to access the innards of the
        // icache, not to take its address, but the latter condition
        // serves as a useful complement to the former.
        MOZ_ASSERT(singleton_->cacheLock_.ownedByCurrentThread());
        return singleton_->icache_;
    }

    static Redirection* redirection() {
        MOZ_ASSERT(singleton_->cacheLock_.ownedByCurrentThread());
        return singleton_->redirection_;
    }

    static void setRedirection(js::jit::Redirection* redirection) {
        MOZ_ASSERT(singleton_->cacheLock_.ownedByCurrentThread());
        singleton_->redirection_ = redirection;
    }
};

} // namespace jit
} // namespace js

#endif /* JS_SIMULATOR_MIPS64 */

#endif /* jit_mips64_Simulator_mips64_h */
