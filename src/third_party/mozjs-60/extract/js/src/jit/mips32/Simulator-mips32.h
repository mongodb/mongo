/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
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

#ifndef jit_mips32_Simulator_mips32_h
#define jit_mips32_Simulator_mips32_h

#ifdef JS_SIMULATOR_MIPS32

#include "mozilla/Atomics.h"

#include "jit/IonTypes.h"
#include "js/ProfilingFrameIterator.h"
#include "threading/Thread.h"
#include "vm/MutexIDs.h"
#include "wasm/WasmCode.h"

namespace js {

namespace jit {

class JitActivation;

class Simulator;
class Redirection;
class CachePage;
class AutoLockSimulator;

const intptr_t kPointerAlignment = 4;
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

// On MIPS Simulator breakpoints can have different codes:
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
        a0, a1, a2, a3,
        t0, t1, t2, t3, t4, t5, t6, t7,
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
        f12, f13, f14, f15,   // f12 and f14 are arguments FPURegisters.
        f16, f17, f18, f19, f20, f21, f22, f23, f24, f25,
        f26, f27, f28, f29, f30, f31,
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
    void setRegister(int reg, int32_t value);
    int32_t getRegister(int reg) const;
    double getDoubleFromRegisterPair(int reg);
    // Same for FPURegisters.
    void setFpuRegister(int fpureg, int32_t value);
    void setFpuRegisterFloat(int fpureg, float value);
    void setFpuRegisterFloat(int fpureg, int64_t value);
    void setFpuRegisterDouble(int fpureg, double value);
    void setFpuRegisterDouble(int fpureg, int64_t value);
    int32_t getFpuRegister(int fpureg) const;
    int64_t getFpuRegisterLong(int fpureg) const;
    float getFpuRegisterFloat(int fpureg) const;
    double getFpuRegisterDouble(int fpureg) const;
    void setFCSRBit(uint32_t cc, bool value);
    bool testFCSRBit(uint32_t cc);
    bool setFCSRRoundError(double original, double rounded);

    // Special case of set_register and get_register to access the raw PC value.
    void set_pc(int32_t value);
    int32_t get_pc() const;

    template <typename T>
    T get_pc_as() const { return reinterpret_cast<T>(get_pc()); }

    void trigger_wasm_interrupt() {
        // This can be called several times if a single interrupt isn't caught
        // and handled by the simulator, but this is fine; once the current
        // instruction is done executing, the interrupt will be handled anyhow.
        wasm_interrupt_ = true;
    }

    // Accessor to the internal simulator stack area.
    uintptr_t stackLimit() const;
    bool overRecursed(uintptr_t newsp = 0) const;
    bool overRecursedWithExtra(uint32_t extra) const;

    // Executes MIPS instructions until the PC reaches end_sim_pc.
    template<bool enableStopSimAt>
    void execute();

    // Sets up the simulator state and grabs the result on return.
    int32_t call(uint8_t* entry, int argument_count, ...);

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
    inline uint32_t readBU(uint32_t addr);
    inline int32_t readB(uint32_t addr);
    inline void writeB(uint32_t addr, uint8_t value);
    inline void writeB(uint32_t addr, int8_t value);

    inline uint16_t readHU(uint32_t addr, SimInstruction* instr);
    inline int16_t readH(uint32_t addr, SimInstruction* instr);
    // Note: Overloaded on the sign of the value.
    inline void writeH(uint32_t addr, uint16_t value, SimInstruction* instr);
    inline void writeH(uint32_t addr, int16_t value, SimInstruction* instr);

    inline int readW(uint32_t addr, SimInstruction* instr);
    inline void writeW(uint32_t addr, int value, SimInstruction* instr);

    inline double readD(uint32_t addr, SimInstruction* instr);
    inline void writeD(uint32_t addr, double value, SimInstruction* instr);

    inline int32_t loadLinkedW(uint32_t addr, SimInstruction* instr);
    inline int32_t storeConditionalW(uint32_t addr, int32_t value, SimInstruction* instr);

    // Executing is handled based on the instruction type.
    void decodeTypeRegister(SimInstruction* instr);

    // Helper function for decodeTypeRegister.
    void configureTypeRegister(SimInstruction* instr,
                               int32_t& alu_out,
                               int64_t& i64hilo,
                               uint64_t& u64hilo,
                               int32_t& next_pc,
                               int32_t& return_addr_reg,
                               bool& do_interrupt);

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
    bool handleWasmFault(int32_t addr, unsigned numBytes);
    bool handleWasmTrapFault();

    // Executes one instruction.
    void instructionDecode(SimInstruction* instr);
    // Execute one instruction placed in a branch delay slot.
    void branchDelayInstructionDecode(SimInstruction* instr);

  public:
    static int StopSimAt;

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

    // Handle arguments and return value for runtime FP functions.
    void getFpArgs(double* x, double* y, int32_t* z);
    void getFpFromStack(int32_t* stack, double* x);

    void setCallResultDouble(double result);
    void setCallResultFloat(float result);
    void setCallResult(int64_t res);

    void callInternal(uint8_t* entry);

    // Architecture state.
    // Registers.
    int32_t registers_[kNumSimuRegisters];
    // Coprocessor Registers.
    int32_t FPUregisters_[kNumFPURegisters];
    // FPU control register.
    uint32_t FCSR_;

    bool LLBit_;
    uint32_t LLAddr_;
    int32_t lastLLValue_;

    // Simulator support.
    char* stack_;
    uintptr_t stackLimit_;
    bool pc_modified_;
    int icount_;
    int break_count_;

    // wasm async interrupt / fault support
    bool wasm_interrupt_;

    // Debugger input.
    char* lastDebuggerInput_;

    // Registered breakpoints.
    SimInstruction* break_pc_;
    Instr break_instr_;

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

#endif /* JS_SIMULATOR_MIPS32 */

#endif /* jit_mips32_Simulator_mips32_h */
