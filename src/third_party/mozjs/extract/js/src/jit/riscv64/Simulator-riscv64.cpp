/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80: */
// Copyright 2021 the V8 project authors. All rights reserved.
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
#ifdef JS_SIMULATOR_RISCV64
#  include "jit/riscv64/Simulator-riscv64.h"

#  include "mozilla/Casting.h"
#  include "mozilla/FloatingPoint.h"
#  include "mozilla/IntegerPrintfMacros.h"
#  include "mozilla/Likely.h"
#  include "mozilla/MathAlgorithms.h"

#  include <float.h>
#  include <iostream>
#  include <limits>

#  include "jit/AtomicOperations.h"
#  include "jit/riscv64/Assembler-riscv64.h"
#  include "js/Conversions.h"
#  include "js/UniquePtr.h"
#  include "js/Utility.h"
#  include "threading/LockGuard.h"
#  include "vm/JSContext.h"
#  include "vm/Runtime.h"
#  include "wasm/WasmInstance.h"
#  include "wasm/WasmSignalHandlers.h"

#  define I8(v) static_cast<int8_t>(v)
#  define I16(v) static_cast<int16_t>(v)
#  define U16(v) static_cast<uint16_t>(v)
#  define I32(v) static_cast<int32_t>(v)
#  define U32(v) static_cast<uint32_t>(v)
#  define I64(v) static_cast<int64_t>(v)
#  define U64(v) static_cast<uint64_t>(v)
#  define I128(v) static_cast<__int128_t>(v)
#  define U128(v) static_cast<__uint128_t>(v)

#  define REGIx_FORMAT PRIx64
#  define REGId_FORMAT PRId64

#  define I32_CHECK(v)                   \
    ({                                   \
      MOZ_ASSERT(I64(I32(v)) == I64(v)); \
      I32((v));                          \
    })

namespace js {
namespace jit {

bool Simulator::FLAG_trace_sim = false;
bool Simulator::FLAG_debug_sim = false;
bool Simulator::FLAG_riscv_trap_to_simulator_debugger = false;
bool Simulator::FLAG_riscv_print_watchpoint = false;

static void UNIMPLEMENTED() {
  printf("UNIMPLEMENTED instruction.\n");
  MOZ_CRASH();
}
static void UNREACHABLE() {
  printf("UNREACHABLE instruction.\n");
  MOZ_CRASH();
}
#  define UNSUPPORTED()                                                \
    std::cout << "Unrecognized instruction [@pc=0x" << std::hex        \
              << registers_[pc] << "]: 0x" << instr_.InstructionBits() \
              << std::endl;                                            \
    printf("Unsupported instruction.\n");                              \
    MOZ_CRASH();

static char* ReadLine(const char* prompt) {
  UniqueChars result;
  char lineBuf[256];
  int offset = 0;
  bool keepGoing = true;
  fprintf(stdout, "%s", prompt);
  fflush(stdout);
  while (keepGoing) {
    if (fgets(lineBuf, sizeof(lineBuf), stdin) == nullptr) {
      // fgets got an error. Just give up.
      return nullptr;
    }
    int len = strlen(lineBuf);
    if (len > 0 && lineBuf[len - 1] == '\n') {
      // Since we read a new line we are done reading the line. This
      // will exit the loop after copying this buffer into the result.
      keepGoing = false;
    }
    if (!result) {
      // Allocate the initial result and make room for the terminating '\0'
      result.reset(js_pod_malloc<char>(len + 1));
      if (!result) {
        return nullptr;
      }
    } else {
      // Allocate a new result with enough room for the new addition.
      int new_len = offset + len + 1;
      char* new_result = js_pod_malloc<char>(new_len);
      if (!new_result) {
        return nullptr;
      }
      // Copy the existing input into the new array and set the new
      // array as the result.
      memcpy(new_result, result.get(), offset * sizeof(char));
      result.reset(new_result);
    }
    // Copy the newly read line into the result.
    memcpy(result.get() + offset, lineBuf, len * sizeof(char));
    offset += len;
  }

  MOZ_ASSERT(result);
  result[offset] = '\0';
  return result.release();
}

// -----------------------------------------------------------------------------
// Riscv assembly various constants.

// C/C++ argument slots size.
const int kCArgSlotCount = 0;
const int kCArgsSlotsSize = kCArgSlotCount * sizeof(uintptr_t);
const int kBranchReturnOffset = 2 * kInstrSize;

class CachePage {
 public:
  static const int LINE_VALID = 0;
  static const int LINE_INVALID = 1;

  static const int kPageShift = 12;
  static const int kPageSize = 1 << kPageShift;
  static const int kPageMask = kPageSize - 1;
  static const int kLineShift = 2;  // The cache line is only 4 bytes right now.
  static const int kLineLength = 1 << kLineShift;
  static const int kLineMask = kLineLength - 1;

  CachePage() { memset(&validity_map_, LINE_INVALID, sizeof(validity_map_)); }

  char* validityByte(int offset) {
    return &validity_map_[offset >> kLineShift];
  }

  char* cachedData(int offset) { return &data_[offset]; }

 private:
  char data_[kPageSize];  // The cached data.
  static const int kValidityMapSize = kPageSize >> kLineShift;
  char validity_map_[kValidityMapSize];  // One byte per line.
};

// Protects the icache() and redirection() properties of the
// Simulator.
class AutoLockSimulatorCache : public LockGuard<Mutex> {
  using Base = LockGuard<Mutex>;

 public:
  explicit AutoLockSimulatorCache()
      : Base(SimulatorProcess::singleton_->cacheLock_) {}
};

mozilla::Atomic<size_t, mozilla::ReleaseAcquire>
    SimulatorProcess::ICacheCheckingDisableCount(
        1);  // Checking is disabled by default.
SimulatorProcess* SimulatorProcess::singleton_ = nullptr;

int64_t Simulator::StopSimAt = -1;

static bool IsFlag(const char* found, const char* flag) {
  return strlen(found) == strlen(flag) && strcmp(found, flag) == 0;
}

Simulator* Simulator::Create() {
  auto sim = MakeUnique<Simulator>();
  if (!sim) {
    return nullptr;
  }

  if (!sim->init()) {
    return nullptr;
  }

  int64_t stopAt;
  char* stopAtStr = getenv("RISCV_SIM_STOP_AT");
  if (stopAtStr && sscanf(stopAtStr, "%" PRIi64, &stopAt) == 1) {
    fprintf(stderr, "\nStopping simulation at icount %" PRIi64 "\n", stopAt);
    Simulator::StopSimAt = stopAt;
  }
  char* str = getenv("RISCV_TRACE_SIM");
  if (str != nullptr && IsFlag(str, "true")) {
    FLAG_trace_sim = true;
  }

  return sim.release();
}

void Simulator::Destroy(Simulator* sim) { js_delete(sim); }

#  if JS_CODEGEN_RISCV64
void Simulator::TraceRegWr(int64_t value, TraceType t) {
  if (FLAG_trace_sim) {
    union {
      int64_t fmt_int64;
      int32_t fmt_int32[2];
      float fmt_float[2];
      double fmt_double;
    } v;
    v.fmt_int64 = value;

    switch (t) {
      case WORD:
        SNPrintF(trace_buf_,
                 "%016" REGIx_FORMAT "    (%" PRId64 ")    int32:%" PRId32
                 " uint32:%" PRIu32,
                 v.fmt_int64, icount_, v.fmt_int32[0], v.fmt_int32[0]);
        break;
      case DWORD:
        SNPrintF(trace_buf_,
                 "%016" REGIx_FORMAT "    (%" PRId64 ")    int64:%" REGId_FORMAT
                 " uint64:%" PRIu64,
                 value, icount_, value, value);
        break;
      case FLOAT:
        SNPrintF(trace_buf_, "%016" REGIx_FORMAT "    (%" PRId64 ")    flt:%e",
                 v.fmt_int64, icount_, v.fmt_float[0]);
        break;
      case DOUBLE:
        SNPrintF(trace_buf_, "%016" REGIx_FORMAT "    (%" PRId64 ")    dbl:%e",
                 v.fmt_int64, icount_, v.fmt_double);
        break;
      default:
        UNREACHABLE();
    }
  }
}

#  elif JS_CODEGEN_RISCV32
template <typename T>
void Simulator::TraceRegWr(T value, TraceType t) {
  if (::v8::internal::FLAG_trace_sim) {
    union {
      int32_t fmt_int32;
      float fmt_float;
      double fmt_double;
    } v;
    if (t != DOUBLE) {
      v.fmt_int32 = value;
    } else {
      DCHECK_EQ(sizeof(T), 8);
      v.fmt_double = value;
    }
    switch (t) {
      case WORD:
        SNPrintF(trace_buf_,
                 "%016" REGIx_FORMAT "    (%" PRId64 ")    int32:%" REGId_FORMAT
                 " uint32:%" PRIu32,
                 v.fmt_int32, icount_, v.fmt_int32, v.fmt_int32);
        break;
      case FLOAT:
        SNPrintF(trace_buf_, "%016" REGIx_FORMAT "    (%" PRId64 ")    flt:%e",
                 v.fmt_int32, icount_, v.fmt_float);
        break;
      case DOUBLE:
        SNPrintF(trace_buf_, "%016" PRIx64 "    (%" PRId64 ")    dbl:%e",
                 static_cast<int64_t>(v.fmt_double), icount_, v.fmt_double);
        break;
      default:
        UNREACHABLE();
    }
  }
}
#  endif
// The RiscvDebugger class is used by the simulator while debugging simulated
// code.
class RiscvDebugger {
 public:
  explicit RiscvDebugger(Simulator* sim) : sim_(sim) {}

  void Debug();
  // Print all registers with a nice formatting.
  void PrintRegs(char name_prefix, int start_index, int end_index);
  void printAllRegs();
  void printAllRegsIncludingFPU();

  static const Instr kNopInstr = 0x0;

 private:
  Simulator* sim_;

  int64_t GetRegisterValue(int regnum);
  int64_t GetFPURegisterValue(int regnum);
  float GetFPURegisterValueFloat(int regnum);
  double GetFPURegisterValueDouble(int regnum);
#  ifdef CAN_USE_RVV_INSTRUCTIONS
  __int128_t GetVRegisterValue(int regnum);
#  endif
  bool GetValue(const char* desc, int64_t* value);
};

int64_t RiscvDebugger::GetRegisterValue(int regnum) {
  if (regnum == Simulator::Register::kNumSimuRegisters) {
    return sim_->get_pc();
  } else {
    return sim_->getRegister(regnum);
  }
}

int64_t RiscvDebugger::GetFPURegisterValue(int regnum) {
  if (regnum == Simulator::FPURegister::kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->getFpuRegister(regnum);
  }
}

float RiscvDebugger::GetFPURegisterValueFloat(int regnum) {
  if (regnum == Simulator::FPURegister::kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->getFpuRegisterFloat(regnum);
  }
}

double RiscvDebugger::GetFPURegisterValueDouble(int regnum) {
  if (regnum == Simulator::FPURegister::kNumFPURegisters) {
    return sim_->get_pc();
  } else {
    return sim_->getFpuRegisterDouble(regnum);
  }
}

#  ifdef CAN_USE_RVV_INSTRUCTIONS
__int128_t RiscvDebugger::GetVRegisterValue(int regnum) {
  if (regnum == kNumVRegisters) {
    return sim_->get_pc();
  } else {
    return sim_->get_vregister(regnum);
  }
}
#  endif

bool RiscvDebugger::GetValue(const char* desc, int64_t* value) {
  int regnum = Registers::FromName(desc);
  int fpuregnum = FloatRegisters::FromName(desc);

  if (regnum != Registers::invalid_reg) {
    *value = GetRegisterValue(regnum);
    return true;
  } else if (fpuregnum != FloatRegisters::invalid_reg) {
    *value = GetFPURegisterValue(fpuregnum);
    return true;
  } else if (strncmp(desc, "0x", 2) == 0) {
    return sscanf(desc + 2, "%" SCNx64, reinterpret_cast<int64_t*>(value)) == 1;
  } else {
    return sscanf(desc, "%" SCNu64, reinterpret_cast<int64_t*>(value)) == 1;
  }
}

#  define REG_INFO(name)                               \
    name, GetRegisterValue(Registers::FromName(name)), \
        GetRegisterValue(Registers::FromName(name))

void RiscvDebugger::PrintRegs(char name_prefix, int start_index,
                              int end_index) {
  EmbeddedVector<char, 10> name1, name2;
  MOZ_ASSERT(name_prefix == 'a' || name_prefix == 't' || name_prefix == 's');
  MOZ_ASSERT(start_index >= 0 && end_index <= 99);
  int num_registers = (end_index - start_index) + 1;
  for (int i = 0; i < num_registers / 2; i++) {
    SNPrintF(name1, "%c%d", name_prefix, start_index + 2 * i);
    SNPrintF(name2, "%c%d", name_prefix, start_index + 2 * i + 1);
    printf("%3s: 0x%016" REGIx_FORMAT "  %14" REGId_FORMAT
           " \t%3s: 0x%016" REGIx_FORMAT "  %14" REGId_FORMAT " \n",
           REG_INFO(name1.start()), REG_INFO(name2.start()));
  }
  if (num_registers % 2 == 1) {
    SNPrintF(name1, "%c%d", name_prefix, end_index);
    printf("%3s: 0x%016" REGIx_FORMAT "  %14" REGId_FORMAT " \n",
           REG_INFO(name1.start()));
  }
}

void RiscvDebugger::printAllRegs() {
  printf("\n");
  // ra, sp, gp
  printf("%3s: 0x%016" REGIx_FORMAT " %14" REGId_FORMAT
         "\t%3s: 0x%016" REGIx_FORMAT " %14" REGId_FORMAT
         "\t%3s: 0x%016" REGIx_FORMAT " %14" REGId_FORMAT "\n",
         REG_INFO("ra"), REG_INFO("sp"), REG_INFO("gp"));

  // tp, fp, pc
  printf("%3s: 0x%016" REGIx_FORMAT " %14" REGId_FORMAT
         "\t%3s: 0x%016" REGIx_FORMAT " %14" REGId_FORMAT
         "\t%3s: 0x%016" REGIx_FORMAT " %14" REGId_FORMAT "\n",
         REG_INFO("tp"), REG_INFO("fp"), REG_INFO("pc"));

  // print register a0, .., a7
  PrintRegs('a', 0, 7);
  // print registers s1, ..., s11
  PrintRegs('s', 1, 11);
  // print registers t0, ..., t6
  PrintRegs('t', 0, 6);
}

#  undef REG_INFO

void RiscvDebugger::printAllRegsIncludingFPU() {
#  define FPU_REG_INFO(n)                               \
    FloatRegisters::GetName(n), GetFPURegisterValue(n), \
        GetFPURegisterValueDouble(n)

  printAllRegs();

  printf("\n\n");
  // f0, f1, f2, ... f31.
  MOZ_ASSERT(kNumFPURegisters % 2 == 0);
  for (int i = 0; i < kNumFPURegisters; i += 2)
    printf("%3s: 0x%016" PRIx64 "  %16.4e \t%3s: 0x%016" PRIx64 "  %16.4e\n",
           FPU_REG_INFO(i), FPU_REG_INFO(i + 1));
#  undef FPU_REG_INFO
}

void RiscvDebugger::Debug() {
  intptr_t last_pc = -1;
  bool done = false;

#  define COMMAND_SIZE 63
#  define ARG_SIZE 255

#  define STR(a) #a
#  define XSTR(a) STR(a)

  char cmd[COMMAND_SIZE + 1];
  char arg1[ARG_SIZE + 1];
  char arg2[ARG_SIZE + 1];
  char* argv[3] = {cmd, arg1, arg2};

  // Make sure to have a proper terminating character if reaching the limit.
  cmd[COMMAND_SIZE] = 0;
  arg1[ARG_SIZE] = 0;
  arg2[ARG_SIZE] = 0;

  while (!done && (sim_->get_pc() != Simulator::end_sim_pc)) {
    if (last_pc != sim_->get_pc()) {
      disasm::NameConverter converter;
      disasm::Disassembler dasm(converter);
      // Use a reasonably large buffer.
      EmbeddedVector<char, 256> buffer;
      dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(sim_->get_pc()));
      printf("  0x%016" REGIx_FORMAT "   %s\n", sim_->get_pc(), buffer.start());
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
      int argc = sscanf(
            line,
            "%" XSTR(COMMAND_SIZE) "s "
            "%" XSTR(ARG_SIZE) "s "
            "%" XSTR(ARG_SIZE) "s",
            cmd, arg1, arg2);
      if ((strcmp(cmd, "si") == 0) || (strcmp(cmd, "stepi") == 0)) {
        SimInstruction* instr =
            reinterpret_cast<SimInstruction*>(sim_->get_pc());
        if (!(instr->IsTrap()) ||
            instr->InstructionBits() == rtCallRedirInstr) {
          sim_->icount_++;
          sim_->InstructionDecode(
              reinterpret_cast<Instruction*>(sim_->get_pc()));
        } else {
          // Allow si to jump over generated breakpoints.
          printf("/!\\ Jumping over generated breakpoint.\n");
          sim_->set_pc(sim_->get_pc() + kInstrSize);
        }
      } else if ((strcmp(cmd, "c") == 0) || (strcmp(cmd, "cont") == 0)) {
        // Leave the debugger shell.
        done = true;
      } else if ((strcmp(cmd, "p") == 0) || (strcmp(cmd, "print") == 0)) {
        if (argc == 2) {
          int64_t value;
          int64_t fvalue;
          double dvalue;
          if (strcmp(arg1, "all") == 0) {
            printAllRegs();
          } else if (strcmp(arg1, "allf") == 0) {
            printAllRegsIncludingFPU();
          } else {
            int regnum = Registers::FromName(arg1);
            int fpuregnum = FloatRegisters::FromName(arg1);
#  ifdef CAN_USE_RVV_INSTRUCTIONS
            int vregnum = VRegisters::FromName(arg1);
#  endif
            if (regnum != Registers::invalid_reg) {
              value = GetRegisterValue(regnum);
              printf("%s: 0x%08" REGIx_FORMAT "  %" REGId_FORMAT "  \n", arg1,
                     value, value);
            } else if (fpuregnum != FloatRegisters::invalid_reg) {
              fvalue = GetFPURegisterValue(fpuregnum);
              dvalue = GetFPURegisterValueDouble(fpuregnum);
              printf("%3s: 0x%016" PRIx64 "  %16.4e\n",
                     FloatRegisters::GetName(fpuregnum), fvalue, dvalue);
#  ifdef CAN_USE_RVV_INSTRUCTIONS
            } else if (vregnum != kInvalidVRegister) {
              __int128_t v = GetVRegisterValue(vregnum);
              printf("\t%s:0x%016" REGIx_FORMAT "%016" REGIx_FORMAT "\n",
                     VRegisters::GetName(vregnum), (uint64_t)(v >> 64),
                     (uint64_t)v);
#  endif
            } else {
              printf("%s unrecognized\n", arg1);
            }
          }
        } else {
          if (argc == 3) {
            if (strcmp(arg2, "single") == 0) {
              int64_t value;
              float fvalue;
              int fpuregnum = FloatRegisters::FromName(arg1);

              if (fpuregnum != FloatRegisters::invalid_reg) {
                value = GetFPURegisterValue(fpuregnum);
                value &= 0xFFFFFFFFUL;
                fvalue = GetFPURegisterValueFloat(fpuregnum);
                printf("%s: 0x%08" PRIx64 "  %11.4e\n", arg1, value, fvalue);
              } else {
                printf("%s unrecognized\n", arg1);
              }
            } else {
              printf("print <fpu register> single\n");
            }
          } else {
            printf("print <register> or print <fpu register> single\n");
          }
        }
      } else if ((strcmp(cmd, "po") == 0) ||
                 (strcmp(cmd, "printobject") == 0)) {
        UNIMPLEMENTED();
      } else if (strcmp(cmd, "stack") == 0 || strcmp(cmd, "mem") == 0) {
        int64_t* cur = nullptr;
        int64_t* end = nullptr;
        int next_arg = 1;
        if (argc < 2) {
          printf("Need to specify <address> to memhex command\n");
          continue;
        }
        int64_t value;
        if (!GetValue(arg1, &value)) {
          printf("%s unrecognized\n", arg1);
          continue;
        }
        cur = reinterpret_cast<int64_t*>(value);
        next_arg++;

        int64_t words;
        if (argc == next_arg) {
          words = 10;
        } else {
          if (!GetValue(argv[next_arg], &words)) {
            words = 10;
          }
        }
        end = cur + words;

        while (cur < end) {
          printf("  0x%012" PRIxPTR " :  0x%016" REGIx_FORMAT
                 "  %14" REGId_FORMAT " ",
                 reinterpret_cast<intptr_t>(cur), *cur, *cur);
          printf("\n");
          cur++;
        }
      } else if ((strcmp(cmd, "watch") == 0)) {
        if (argc < 2) {
          printf("Need to specify <address> to mem command\n");
          continue;
        }
        int64_t value;
        if (!GetValue(arg1, &value)) {
          printf("%s unrecognized\n", arg1);
          continue;
        }
        sim_->watch_address_ = reinterpret_cast<int64_t*>(value);
        sim_->watch_value_ = *(sim_->watch_address_);
      } else if ((strcmp(cmd, "disasm") == 0) || (strcmp(cmd, "dpc") == 0) ||
                 (strcmp(cmd, "di") == 0)) {
        disasm::NameConverter converter;
        disasm::Disassembler dasm(converter);
        // Use a reasonably large buffer.
        EmbeddedVector<char, 256> buffer;

        byte* cur = nullptr;
        byte* end = nullptr;

        if (argc == 1) {
          cur = reinterpret_cast<byte*>(sim_->get_pc());
          end = cur + (10 * kInstrSize);
        } else if (argc == 2) {
          auto regnum = Registers::FromName(arg1);
          if (regnum != Registers::invalid_reg || strncmp(arg1, "0x", 2) == 0) {
            // The argument is an address or a register name.
            sreg_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(value);
              // Disassemble 10 instructions at <arg1>.
              end = cur + (10 * kInstrSize);
            }
          } else {
            // The argument is the number of instructions.
            sreg_t value;
            if (GetValue(arg1, &value)) {
              cur = reinterpret_cast<byte*>(sim_->get_pc());
              // Disassemble <arg1> instructions.
              end = cur + (value * kInstrSize);
            }
          }
        } else {
          sreg_t value1;
          sreg_t value2;
          if (GetValue(arg1, &value1) && GetValue(arg2, &value2)) {
            cur = reinterpret_cast<byte*>(value1);
            end = cur + (value2 * kInstrSize);
          }
        }
        while (cur < end) {
          dasm.InstructionDecode(buffer, cur);
          printf("  0x%08" PRIxPTR "   %s\n", reinterpret_cast<intptr_t>(cur),
                 buffer.start());
          cur += kInstrSize;
        }
      } else if (strcmp(cmd, "trace") == 0) {
        Simulator::FLAG_trace_sim = true;
        Simulator::FLAG_riscv_print_watchpoint = true;
      } else if (strcmp(cmd, "break") == 0 || strcmp(cmd, "b") == 0 ||
                 strcmp(cmd, "tbreak") == 0) {
        bool is_tbreak = strcmp(cmd, "tbreak") == 0;
        if (argc == 2) {
          int64_t value;
          if (GetValue(arg1, &value)) {
            sim_->SetBreakpoint(reinterpret_cast<SimInstruction*>(value),
                                is_tbreak);
          } else {
            printf("%s unrecognized\n", arg1);
          }
        } else {
          sim_->ListBreakpoints();
          printf("Use `break <address>` to set or disable a breakpoint\n");
          printf(
              "Use `tbreak <address>` to set or disable a temporary "
              "breakpoint\n");
        }
      } else if (strcmp(cmd, "flags") == 0) {
        printf("No flags on RISC-V !\n");
      } else if (strcmp(cmd, "stop") == 0) {
        int64_t value;
        if (argc == 3) {
          // Print information about all/the specified breakpoint(s).
          if (strcmp(arg1, "info") == 0) {
            if (strcmp(arg2, "all") == 0) {
              printf("Stop information:\n");
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->printStopInfo(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->printStopInfo(value);
            } else {
              printf("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "enable") == 0) {
            // Enable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->enableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->enableStop(value);
            } else {
              printf("Unrecognized argument.\n");
            }
          } else if (strcmp(arg1, "disable") == 0) {
            // Disable all/the specified breakpoint(s).
            if (strcmp(arg2, "all") == 0) {
              for (uint32_t i = kMaxWatchpointCode + 1; i <= kMaxStopCode;
                   i++) {
                sim_->disableStop(i);
              }
            } else if (GetValue(arg2, &value)) {
              sim_->disableStop(value);
            } else {
              printf("Unrecognized argument.\n");
            }
          }
        } else {
          printf("Wrong usage. Use help command for more information.\n");
        }
      } else if ((strcmp(cmd, "stat") == 0) || (strcmp(cmd, "st") == 0)) {
        UNIMPLEMENTED();
      } else if ((strcmp(cmd, "h") == 0) || (strcmp(cmd, "help") == 0)) {
        printf("cont (alias 'c')\n");
        printf("  Continue execution\n");
        printf("stepi (alias 'si')\n");
        printf("  Step one instruction\n");
        printf("print (alias 'p')\n");
        printf("  print <register>\n");
        printf("  Print register content\n");
        printf("  Use register name 'all' to print all GPRs\n");
        printf("  Use register name 'allf' to print all GPRs and FPRs\n");
        printf("printobject (alias 'po')\n");
        printf("  printobject <register>\n");
        printf("  Print an object from a register\n");
        printf("stack\n");
        printf("  stack [<words>]\n");
        printf("  Dump stack content, default dump 10 words)\n");
        printf("mem\n");
        printf("  mem <address> [<words>]\n");
        printf("  Dump memory content, default dump 10 words)\n");
        printf("watch\n");
        printf("  watch <address> \n");
        printf("  watch memory content.)\n");
        printf("flags\n");
        printf("  print flags\n");
        printf("disasm (alias 'di')\n");
        printf("  disasm [<instructions>]\n");
        printf("  disasm [<address/register>] (e.g., disasm pc) \n");
        printf("  disasm [[<address/register>] <instructions>]\n");
        printf("  Disassemble code, default is 10 instructions\n");
        printf("  from pc\n");
        printf("gdb \n");
        printf("  Return to gdb if the simulator was started with gdb\n");
        printf("break (alias 'b')\n");
        printf("  break : list all breakpoints\n");
        printf("  break <address> : set / enable / disable a breakpoint.\n");
        printf("tbreak\n");
        printf("  tbreak : list all breakpoints\n");
        printf(
            "  tbreak <address> : set / enable / disable a temporary "
            "breakpoint.\n");
        printf("  Set a breakpoint enabled only for one stop. \n");
        printf("stop feature:\n");
        printf("  Description:\n");
        printf("    Stops are debug instructions inserted by\n");
        printf("    the Assembler::stop() function.\n");
        printf("    When hitting a stop, the Simulator will\n");
        printf("    stop and give control to the Debugger.\n");
        printf("    All stop codes are watched:\n");
        printf("    - They can be enabled / disabled: the Simulator\n");
        printf("       will / won't stop when hitting them.\n");
        printf("    - The Simulator keeps track of how many times they \n");
        printf("      are met. (See the info command.) Going over a\n");
        printf("      disabled stop still increases its counter. \n");
        printf("  Commands:\n");
        printf("    stop info all/<code> : print infos about number <code>\n");
        printf("      or all stop(s).\n");
        printf("    stop enable/disable all/<code> : enables / disables\n");
        printf("      all or number <code> stop(s)\n");
      } else {
        printf("Unknown command: %s\n", cmd);
      }
    }
  }

#  undef COMMAND_SIZE
#  undef ARG_SIZE

#  undef STR
#  undef XSTR
}

void Simulator::SetBreakpoint(SimInstruction* location, bool is_tbreak) {
  for (unsigned i = 0; i < breakpoints_.size(); i++) {
    if (breakpoints_.at(i).location == location) {
      if (breakpoints_.at(i).is_tbreak != is_tbreak) {
        printf("Change breakpoint at %p to %s breakpoint\n",
               reinterpret_cast<void*>(location),
               is_tbreak ? "temporary" : "regular");
        breakpoints_.at(i).is_tbreak = is_tbreak;
        return;
      }
      printf("Existing breakpoint at %p was %s\n",
             reinterpret_cast<void*>(location),
             breakpoints_.at(i).enabled ? "disabled" : "enabled");
      breakpoints_.at(i).enabled = !breakpoints_.at(i).enabled;
      return;
    }
  }
  Breakpoint new_breakpoint = {location, true, is_tbreak};
  breakpoints_.push_back(new_breakpoint);
  printf("Set a %sbreakpoint at %p\n", is_tbreak ? "temporary " : "",
         reinterpret_cast<void*>(location));
}

void Simulator::ListBreakpoints() {
  printf("Breakpoints:\n");
  for (unsigned i = 0; i < breakpoints_.size(); i++) {
    printf("%p  : %s %s\n",
           reinterpret_cast<void*>(breakpoints_.at(i).location),
           breakpoints_.at(i).enabled ? "enabled" : "disabled",
           breakpoints_.at(i).is_tbreak ? ": temporary" : "");
  }
}

void Simulator::CheckBreakpoints() {
  bool hit_a_breakpoint = false;
  bool is_tbreak = false;
  SimInstruction* pc_ = reinterpret_cast<SimInstruction*>(get_pc());
  for (unsigned i = 0; i < breakpoints_.size(); i++) {
    if ((breakpoints_.at(i).location == pc_) && breakpoints_.at(i).enabled) {
      hit_a_breakpoint = true;
      if (breakpoints_.at(i).is_tbreak) {
        // Disable a temporary breakpoint.
        is_tbreak = true;
        breakpoints_.at(i).enabled = false;
      }
      break;
    }
  }
  if (hit_a_breakpoint) {
    printf("Hit %sa breakpoint at %p.\n", is_tbreak ? "and disabled " : "",
           reinterpret_cast<void*>(pc_));
    RiscvDebugger dbg(this);
    dbg.Debug();
  }
}

static bool AllOnOnePage(uintptr_t start, int size) {
  intptr_t start_page = (start & ~CachePage::kPageMask);
  intptr_t end_page = ((start + size) & ~CachePage::kPageMask);
  return start_page == end_page;
}

void Simulator::setLastDebuggerInput(char* input) {
  js_free(lastDebuggerInput_);
  lastDebuggerInput_ = input;
}

static CachePage* GetCachePageLocked(SimulatorProcess::ICacheMap& i_cache,
                                     void* page) {
  SimulatorProcess::ICacheMap::AddPtr p = i_cache.lookupForAdd(page);
  if (p) {
    return p->value();
  }
  AutoEnterOOMUnsafeRegion oomUnsafe;
  CachePage* new_page = js_new<CachePage>();
  if (!new_page || !i_cache.add(p, page, new_page)) {
    oomUnsafe.crash("Simulator CachePage");
  }
  return new_page;
}

// Flush from start up to and not including start + size.
static void FlushOnePageLocked(SimulatorProcess::ICacheMap& i_cache,
                               intptr_t start, int size) {
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

static void FlushICacheLocked(SimulatorProcess::ICacheMap& i_cache,
                              void* start_addr, size_t size) {
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
  if (size != 0) {
    FlushOnePageLocked(i_cache, start, size);
  }
}

/* static */
void SimulatorProcess::checkICacheLocked(SimInstruction* instr) {
  intptr_t address = reinterpret_cast<intptr_t>(instr);
  void* page = reinterpret_cast<void*>(address & (~CachePage::kPageMask));
  void* line = reinterpret_cast<void*>(address & (~CachePage::kLineMask));
  int offset = (address & CachePage::kPageMask);
  CachePage* cache_page = GetCachePageLocked(icache(), page);
  char* cache_valid_byte = cache_page->validityByte(offset);
  bool cache_hit = (*cache_valid_byte == CachePage::LINE_VALID);
  char* cached_line = cache_page->cachedData(offset & ~CachePage::kLineMask);

  if (cache_hit) {
    // Check that the data in memory matches the contents of the I-cache.
    int cmpret = memcmp(reinterpret_cast<void*>(instr),
                        cache_page->cachedData(offset), kInstrSize);
    MOZ_ASSERT(cmpret == 0);
  } else {
    // Cache miss.  Load memory into the cache.
    memcpy(cached_line, line, CachePage::kLineLength);
    *cache_valid_byte = CachePage::LINE_VALID;
  }
}

HashNumber SimulatorProcess::ICacheHasher::hash(const Lookup& l) {
  return U32(reinterpret_cast<uintptr_t>(l)) >> 2;
}

bool SimulatorProcess::ICacheHasher::match(const Key& k, const Lookup& l) {
  MOZ_ASSERT((reinterpret_cast<intptr_t>(k) & CachePage::kPageMask) == 0);
  MOZ_ASSERT((reinterpret_cast<intptr_t>(l) & CachePage::kPageMask) == 0);
  return k == l;
}

/* static */
void SimulatorProcess::FlushICache(void* start_addr, size_t size) {
  if (!ICacheCheckingDisableCount) {
    AutoLockSimulatorCache als;
    js::jit::FlushICacheLocked(icache(), start_addr, size);
  }
}

Simulator::Simulator() {
  // Set up simulator support first. Some of this information is needed to
  // setup the architecture state.

  // Note, allocation and anything that depends on allocated memory is
  // deferred until init(), in order to handle OOM properly.

  stack_ = nullptr;
  stackLimit_ = 0;
  pc_modified_ = false;
  icount_ = 0;
  break_count_ = 0;
  break_pc_ = nullptr;
  break_instr_ = 0;
  single_stepping_ = false;
  single_step_callback_ = nullptr;
  single_step_callback_arg_ = nullptr;

  // Set up architecture state.
  // All registers are initialized to zero to start with.
  for (int i = 0; i < Simulator::Register::kNumSimuRegisters; i++) {
    registers_[i] = 0;
  }
  for (int i = 0; i < Simulator::FPURegister::kNumFPURegisters; i++) {
    FPUregisters_[i] = 0;
  }
  FCSR_ = 0;
  LLBit_ = false;
  LLAddr_ = 0;
  lastLLValue_ = 0;

  // The ra and pc are initialized to a known bad value that will cause an
  // access violation if the simulator ever tries to execute it.
  registers_[pc] = bad_ra;
  registers_[ra] = bad_ra;

  for (int i = 0; i < kNumExceptions; i++) {
    exceptions[i] = 0;
  }

  lastDebuggerInput_ = nullptr;
}

bool Simulator::init() {
  // Allocate 2MB for the stack. Note that we will only use 1MB, see below.
  static const size_t stackSize = 2 * 1024 * 1024;
  stack_ = js_pod_malloc<char>(stackSize);
  if (!stack_) {
    return false;
  }

  // Leave a safety margin of 1MB to prevent overrunning the stack when
  // pushing values (total stack size is 2MB).
  stackLimit_ = reinterpret_cast<uintptr_t>(stack_) + 1024 * 1024;

  // The sp is initialized to point to the bottom (high address) of the
  // allocated stack area. To be safe in potential stack underflows we leave
  // some buffer below.
  registers_[sp] = reinterpret_cast<int64_t>(stack_) + stackSize - 64;

  return true;
}

// When the generated code calls an external reference we need to catch that in
// the simulator.  The external reference will be a function compiled for the
// host architecture.  We need to call that function instead of trying to
// execute it with the simulator.  We do that by redirecting the external
// reference to a swi (software-interrupt) instruction that is handled by
// the simulator.  We write the original destination of the jump just at a known
// offset from the swi instruction so the simulator knows what to call.
class Redirection {
  friend class SimulatorProcess;

  // sim's lock must already be held.
  Redirection(void* nativeFunction, ABIFunctionType type)
      : nativeFunction_(nativeFunction),
        swiInstruction_(rtCallRedirInstr),
        type_(type),
        next_(nullptr) {
    next_ = SimulatorProcess::redirection();
    if (!SimulatorProcess::ICacheCheckingDisableCount) {
      FlushICacheLocked(SimulatorProcess::icache(), addressOfSwiInstruction(),
                        kInstrSize);
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

    // Note: we can't use js_new here because the constructor is private.
    AutoEnterOOMUnsafeRegion oomUnsafe;
    Redirection* redir = js_pod_malloc<Redirection>(1);
    if (!redir) {
      oomUnsafe.crash("Simulator redirection");
    }
    new (redir) Redirection(nativeFunction, type);
    return redir;
  }

  static Redirection* FromSwiInstruction(Instruction* swiInstruction) {
    uint8_t* addrOfSwi = reinterpret_cast<uint8_t*>(swiInstruction);
    uint8_t* addrOfRedirection =
        addrOfSwi - offsetof(Redirection, swiInstruction_);
    return reinterpret_cast<Redirection*>(addrOfRedirection);
  }

 private:
  void* nativeFunction_;
  uint32_t swiInstruction_;
  ABIFunctionType type_;
  Redirection* next_;
};

Simulator::~Simulator() { js_free(stack_); }

SimulatorProcess::SimulatorProcess()
    : cacheLock_(mutexid::SimulatorCacheLock), redirection_(nullptr) {
  if (getenv("MIPS_SIM_ICACHE_CHECKS")) {
    ICacheCheckingDisableCount = 0;
  }
}

SimulatorProcess::~SimulatorProcess() {
  Redirection* r = redirection_;
  while (r) {
    Redirection* next = r->next_;
    js_delete(r);
    r = next;
  }
}

/* static */
void* Simulator::RedirectNativeFunction(void* nativeFunction,
                                        ABIFunctionType type) {
  Redirection* redirection = Redirection::Get(nativeFunction, type);
  return redirection->addressOfSwiInstruction();
}

// Get the active Simulator for the current thread.
Simulator* Simulator::Current() {
  JSContext* cx = TlsContext.get();
  MOZ_ASSERT(CurrentThreadCanAccessRuntime(cx->runtime()));
  return cx->simulator();
}

// Sets the register in the architecture state. It will also deal with updating
// Simulator internal state for special registers such as PC.
void Simulator::setRegister(int reg, int64_t value) {
  MOZ_ASSERT((reg >= 0) && (reg < Simulator::Register::kNumSimuRegisters));
  if (reg == pc) {
    pc_modified_ = true;
  }

  // Zero register always holds 0.
  registers_[reg] = (reg == 0) ? 0 : value;
}

void Simulator::setFpuRegister(int fpureg, int64_t value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  FPUregisters_[fpureg] = value;
}

void Simulator::setFpuRegisterLo(int fpureg, int32_t value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  *mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg]) = value;
}

void Simulator::setFpuRegisterHi(int fpureg, int32_t value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  *((mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg])) + 1) = value;
}

void Simulator::setFpuRegisterFloat(int fpureg, float value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  *mozilla::BitwiseCast<int64_t*>(&FPUregisters_[fpureg]) = box_float(value);
}

void Simulator::setFpuRegisterFloat(int fpureg, Float32 value) {
  MOZ_ASSERT((fpureg >= 0) && (fpureg < kNumFPURegisters));
  Float64 t = Float64::FromBits(box_float(value.get_bits()));
  memcpy(&FPUregisters_[fpureg], &t, 8);
}

void Simulator::setFpuRegisterDouble(int fpureg, double value) {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  *mozilla::BitwiseCast<double*>(&FPUregisters_[fpureg]) = value;
}

void Simulator::setFpuRegisterDouble(int fpureg, Float64 value) {
  MOZ_ASSERT((fpureg >= 0) && (fpureg < kNumFPURegisters));
  memcpy(&FPUregisters_[fpureg], &value, 8);
}

// Get the register from the architecture state. This function does handle
// the special case of accessing the PC register.
int64_t Simulator::getRegister(int reg) const {
  MOZ_ASSERT((reg >= 0) && (reg < Simulator::Register::kNumSimuRegisters));
  if (reg == 0) {
    return 0;
  }
  return registers_[reg] + ((reg == pc) ? SimInstruction::kPCReadOffset : 0);
}

int64_t Simulator::getFpuRegister(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return FPUregisters_[fpureg];
}

int32_t Simulator::getFpuRegisterLo(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg]);
}

int32_t Simulator::getFpuRegisterHi(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *((mozilla::BitwiseCast<int32_t*>(&FPUregisters_[fpureg])) + 1);
}

float Simulator::getFpuRegisterFloat(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<float*>(&FPUregisters_[fpureg]);
}

Float32 Simulator::getFpuRegisterFloat32(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) && (fpureg < kNumFPURegisters));
  if (!is_boxed_float(FPUregisters_[fpureg])) {
    return Float32::FromBits(0x7ffc0000);
  }
  return Float32::FromBits(
      *bit_cast<uint32_t*>(const_cast<int64_t*>(&FPUregisters_[fpureg])));
}

double Simulator::getFpuRegisterDouble(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) &&
             (fpureg < Simulator::FPURegister::kNumFPURegisters));
  return *mozilla::BitwiseCast<double*>(&FPUregisters_[fpureg]);
}

Float64 Simulator::getFpuRegisterFloat64(int fpureg) const {
  MOZ_ASSERT((fpureg >= 0) && (fpureg < kNumFPURegisters));
  return Float64::FromBits(FPUregisters_[fpureg]);
}

void Simulator::setCallResultDouble(double result) {
  setFpuRegisterDouble(fa0, result);
}

void Simulator::setCallResultFloat(float result) {
  setFpuRegisterFloat(fa0, result);
}

void Simulator::setCallResult(int64_t res) { setRegister(a0, res); }

void Simulator::setCallResult(__int128_t res) {
  setRegister(a0, I64(res));
  setRegister(a1, I64(res >> 64));
}

// Raw access to the PC register.
void Simulator::set_pc(int64_t value) {
  pc_modified_ = true;
  registers_[pc] = value;
}

bool Simulator::has_bad_pc() const {
  return ((registers_[pc] == bad_ra) || (registers_[pc] == end_sim_pc));
}

// Raw access to the PC register without the special adjustment when reading.
int64_t Simulator::get_pc() const { return registers_[pc]; }

JS::ProfilingFrameIterator::RegisterState Simulator::registerState() {
  wasm::RegisterState state;
  state.pc = (void*)get_pc();
  state.fp = (void*)getRegister(fp);
  state.sp = (void*)getRegister(sp);
  state.lr = (void*)getRegister(ra);
  return state;
}

void Simulator::HandleWasmTrap() {
  uint8_t* newPC;
  if (wasm::HandleIllegalInstruction(registerState(), &newPC)) {
    set_pc(int64_t(newPC));
    return;
  }
}

// TODO(plind): consider making icount_ printing a flag option.
template <typename T>
void Simulator::TraceMemRd(sreg_t addr, T value, sreg_t reg_value) {
  if (FLAG_trace_sim) {
    if (std::is_integral<T>::value) {
      switch (sizeof(T)) {
        case 1:
          SNPrintF(trace_buf_,
                   "%016" REGIx_FORMAT "    (%" PRId64 ")    int8:%" PRId8
                   " uint8:%" PRIu8 " <-- [addr: %" REGIx_FORMAT "]",
                   reg_value, icount_, static_cast<int8_t>(value),
                   static_cast<uint8_t>(value), addr);
          break;
        case 2:
          SNPrintF(trace_buf_,
                   "%016" REGIx_FORMAT "    (%" PRId64 ")    int16:%" PRId16
                   " uint16:%" PRIu16 " <-- [addr: %" REGIx_FORMAT "]",
                   reg_value, icount_, static_cast<int16_t>(value),
                   static_cast<uint16_t>(value), addr);
          break;
        case 4:
          SNPrintF(trace_buf_,
                   "%016" REGIx_FORMAT "    (%" PRId64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " <-- [addr: %" REGIx_FORMAT "]",
                   reg_value, icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
          break;
        case 8:
          SNPrintF(trace_buf_,
                   "%016" REGIx_FORMAT "    (%" PRId64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " <-- [addr: %" REGIx_FORMAT "]",
                   reg_value, icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
          break;
        default:
          UNREACHABLE();
      }
    } else if (std::is_same<float, T>::value) {
      SNPrintF(trace_buf_,
               "%016" REGIx_FORMAT "    (%" PRId64
               ")    flt:%e <-- [addr: %" REGIx_FORMAT "]",
               reg_value, icount_, static_cast<float>(value), addr);
    } else if (std::is_same<double, T>::value) {
      SNPrintF(trace_buf_,
               "%016" REGIx_FORMAT "    (%" PRId64
               ")    dbl:%e <-- [addr: %" REGIx_FORMAT "]",
               reg_value, icount_, static_cast<double>(value), addr);
    } else {
      UNREACHABLE();
    }
  }
}

void Simulator::TraceMemRdFloat(sreg_t addr, Float32 value, int64_t reg_value) {
  if (FLAG_trace_sim) {
    SNPrintF(trace_buf_,
             "%016" PRIx64 "    (%" PRId64
             ")    flt:%e <-- [addr: %" REGIx_FORMAT "]",
             reg_value, icount_, static_cast<float>(value.get_scalar()), addr);
  }
}

void Simulator::TraceMemRdDouble(sreg_t addr, double value, int64_t reg_value) {
  if (FLAG_trace_sim) {
    SNPrintF(trace_buf_,
             "%016" PRIx64 "    (%" PRId64
             ")    dbl:%e <-- [addr: %" REGIx_FORMAT "]",
             reg_value, icount_, static_cast<double>(value), addr);
  }
}

void Simulator::TraceMemRdDouble(sreg_t addr, Float64 value,
                                 int64_t reg_value) {
  if (FLAG_trace_sim) {
    SNPrintF(trace_buf_,
             "%016" PRIx64 "    (%" PRId64
             ")    dbl:%e <-- [addr: %" REGIx_FORMAT "]",
             reg_value, icount_, static_cast<double>(value.get_scalar()), addr);
  }
}

template <typename T>
void Simulator::TraceMemWr(sreg_t addr, T value) {
  if (FLAG_trace_sim) {
    switch (sizeof(T)) {
      case 1:
        SNPrintF(trace_buf_,
                 "                    (%" PRIu64 ")    int8:%" PRId8
                 " uint8:%" PRIu8 " --> [addr: %" REGIx_FORMAT "]",
                 icount_, static_cast<int8_t>(value),
                 static_cast<uint8_t>(value), addr);
        break;
      case 2:
        SNPrintF(trace_buf_,
                 "                    (%" PRIu64 ")    int16:%" PRId16
                 " uint16:%" PRIu16 " --> [addr: %" REGIx_FORMAT "]",
                 icount_, static_cast<int16_t>(value),
                 static_cast<uint16_t>(value), addr);
        break;
      case 4:
        if (std::is_integral<T>::value) {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " --> [addr: %" REGIx_FORMAT "]",
                   icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
        } else {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64
                   ")    flt:%e --> [addr: %" REGIx_FORMAT "]",
                   icount_, static_cast<float>(value), addr);
        }
        break;
      case 8:
        if (std::is_integral<T>::value) {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " --> [addr: %" REGIx_FORMAT "]",
                   icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
        } else {
          SNPrintF(trace_buf_,
                   "                    (%" PRIu64
                   ")    dbl:%e --> [addr: %" REGIx_FORMAT "]",
                   icount_, static_cast<double>(value), addr);
        }
        break;
      default:
        UNREACHABLE();
    }
  }
}

void Simulator::TraceMemWrDouble(sreg_t addr, double value) {
  if (FLAG_trace_sim) {
    SNPrintF(trace_buf_,
             "                    (%" PRIu64
             ")    dbl:%e --> [addr: %" REGIx_FORMAT "]",
             icount_, value, addr);
  }
}

template <typename T>
void Simulator::TraceLr(sreg_t addr, T value, sreg_t reg_value) {
  if (FLAG_trace_sim) {
    if (std::is_integral<T>::value) {
      switch (sizeof(T)) {
        case 4:
          SNPrintF(trace_buf_,
                   "%016" REGIx_FORMAT "    (%" PRId64 ")    int32:%" PRId32
                   " uint32:%" PRIu32 " <-- [addr: %" REGIx_FORMAT "]",
                   reg_value, icount_, static_cast<int32_t>(value),
                   static_cast<uint32_t>(value), addr);
          break;
        case 8:
          SNPrintF(trace_buf_,
                   "%016" REGIx_FORMAT "    (%" PRId64 ")    int64:%" PRId64
                   " uint64:%" PRIu64 " <-- [addr: %" REGIx_FORMAT "]",
                   reg_value, icount_, static_cast<int64_t>(value),
                   static_cast<uint64_t>(value), addr);
          break;
        default:
          UNREACHABLE();
      }
    } else {
      UNREACHABLE();
    }
  }
}

template <typename T>
void Simulator::TraceSc(sreg_t addr, T value) {
  if (FLAG_trace_sim) {
    switch (sizeof(T)) {
      case 4:
        SNPrintF(trace_buf_,
                 "%016" REGIx_FORMAT "    (%" PRIu64 ")    int32:%" PRId32
                 " uint32:%" PRIu32 " --> [addr: %" REGIx_FORMAT "]",
                 getRegister(rd_reg()), icount_, static_cast<int32_t>(value),
                 static_cast<uint32_t>(value), addr);
        break;
      case 8:
        SNPrintF(trace_buf_,
                 "%016" REGIx_FORMAT "    (%" PRIu64 ")    int64:%" PRId64
                 " uint64:%" PRIu64 " --> [addr: %" REGIx_FORMAT "]",
                 getRegister(rd_reg()), icount_, static_cast<int64_t>(value),
                 static_cast<uint64_t>(value), addr);
        break;
      default:
        UNREACHABLE();
    }
  }
}

// TODO(RISCV): check whether the specific board supports unaligned load/store
// (determined by EEI). For now, we assume the board does not support unaligned
// load/store (e.g., trapping)
template <typename T>
T Simulator::ReadMem(sreg_t addr, Instruction* instr) {
  if (handleWasmSegFault(addr, sizeof(T))) {
    return -1;
  }
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    printf("Memory read from bad address: 0x%08" REGIx_FORMAT
           " , pc=0x%08" PRIxPTR " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
  T* ptr = reinterpret_cast<T*>(addr);
  T value = *ptr;
  return value;
}

template <typename T>
void Simulator::WriteMem(sreg_t addr, T value, Instruction* instr) {
  if (handleWasmSegFault(addr, sizeof(T))) {
    value = -1;
    return;
  }
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    printf("Memory write to bad address: 0x%08" REGIx_FORMAT
           " , pc=0x%08" PRIxPTR " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
  T* ptr = reinterpret_cast<T*>(addr);
  if (!std::is_same<double, T>::value) {
    TraceMemWr(addr, value);
  } else {
    TraceMemWrDouble(addr, value);
  }
  *ptr = value;
}

template <>
void Simulator::WriteMem(sreg_t addr, Float32 value, Instruction* instr) {
  if (handleWasmSegFault(addr, 4)) {
    value = Float32(-1.0f);
    return;
  }
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    printf("Memory write to bad address: 0x%08" REGIx_FORMAT
           " , pc=0x%08" PRIxPTR " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
  float* ptr = reinterpret_cast<float*>(addr);
  TraceMemWr(addr, value.get_scalar());
  memcpy(ptr, &value, 4);
}

template <>
void Simulator::WriteMem(sreg_t addr, Float64 value, Instruction* instr) {
  if (handleWasmSegFault(addr, 8)) {
    value = Float64(-1.0);
    return;
  }
  if (addr >= 0 && addr < 0x400) {
    // This has to be a nullptr-dereference, drop into debugger.
    printf("Memory write to bad address: 0x%08" REGIx_FORMAT
           " , pc=0x%08" PRIxPTR " \n",
           addr, reinterpret_cast<intptr_t>(instr));
    DieOrDebug();
  }
  double* ptr = reinterpret_cast<double*>(addr);
  TraceMemWrDouble(addr, value.get_scalar());
  memcpy(ptr, &value, 8);
}

uintptr_t Simulator::stackLimit() const { return stackLimit_; }

uintptr_t* Simulator::addressOfStackLimit() { return &stackLimit_; }

bool Simulator::overRecursed(uintptr_t newsp) const {
  if (newsp == 0) {
    newsp = getRegister(sp);
  }
  return newsp <= stackLimit();
}

bool Simulator::overRecursedWithExtra(uint32_t extra) const {
  uintptr_t newsp = getRegister(sp) - extra;
  return newsp <= stackLimit();
}

// Unsupported instructions use format to print an error and stop execution.
void Simulator::format(SimInstruction* instr, const char* format) {
  printf("Simulator found unsupported instruction:\n 0x%016lx: %s\n",
         reinterpret_cast<intptr_t>(instr), format);
  MOZ_CRASH();
}

// Note: With the code below we assume that all runtime calls return a 64 bits
// result. If they don't, the v1 result register contains a bogus value, which
// is fine because it is caller-saved.
typedef int64_t (*Prototype_General0)();
typedef int64_t (*Prototype_General1)(int64_t arg0);
typedef int64_t (*Prototype_General2)(int64_t arg0, int64_t arg1);
typedef int64_t (*Prototype_General3)(int64_t arg0, int64_t arg1, int64_t arg2);
typedef int64_t (*Prototype_General4)(int64_t arg0, int64_t arg1, int64_t arg2,
                                      int64_t arg3);
typedef int64_t (*Prototype_General5)(int64_t arg0, int64_t arg1, int64_t arg2,
                                      int64_t arg3, int64_t arg4);
typedef int64_t (*Prototype_General6)(int64_t arg0, int64_t arg1, int64_t arg2,
                                      int64_t arg3, int64_t arg4, int64_t arg5);
typedef int64_t (*Prototype_General7)(int64_t arg0, int64_t arg1, int64_t arg2,
                                      int64_t arg3, int64_t arg4, int64_t arg5,
                                      int64_t arg6);
typedef int64_t (*Prototype_General8)(int64_t arg0, int64_t arg1, int64_t arg2,
                                      int64_t arg3, int64_t arg4, int64_t arg5,
                                      int64_t arg6, int64_t arg7);
typedef int64_t (*Prototype_GeneralGeneralGeneralInt64)(int64_t arg0,
                                                        int64_t arg1,
                                                        int64_t arg2,
                                                        int64_t arg3);
typedef int64_t (*Prototype_GeneralGeneralInt64Int64)(int64_t arg0,
                                                      int64_t arg1,
                                                      int64_t arg2,
                                                      int64_t arg3);

typedef int64_t (*Prototype_Int_Double)(double arg0);
typedef int64_t (*Prototype_Int_IntDouble)(int64_t arg0, double arg1);
typedef int64_t (*Prototype_Int_DoubleInt)(double arg0, int64_t arg1);
typedef int64_t (*Prototype_Int_DoubleIntInt)(double arg0, int64_t arg1,
                                              int64_t arg2);
typedef int64_t (*Prototype_Int_IntDoubleIntInt)(int64_t arg0, double arg1,
                                                 int64_t arg2, int64_t arg3);

typedef float (*Prototype_Float32_Float32)(float arg0);
typedef int64_t (*Prototype_Int_Float32)(float arg0);
typedef float (*Prototype_Float32_Float32Float32)(float arg0, float arg1);

typedef double (*Prototype_Double_None)();
typedef double (*Prototype_Double_Double)(double arg0);
typedef double (*Prototype_Double_Int)(int64_t arg0);
typedef double (*Prototype_Double_DoubleInt)(double arg0, int64_t arg1);
typedef double (*Prototype_Double_IntDouble)(int64_t arg0, double arg1);
typedef double (*Prototype_Double_DoubleDouble)(double arg0, double arg1);
typedef double (*Prototype_Double_DoubleDoubleDouble)(double arg0, double arg1,
                                                      double arg2);
typedef double (*Prototype_Double_DoubleDoubleDoubleDouble)(double arg0,
                                                            double arg1,
                                                            double arg2,
                                                            double arg3);

typedef int32_t (*Prototype_Int32_General)(int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt32)(int64_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32)(int64_t, int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32Int32)(int64_t, int32_t,
                                                          int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32Int32Int32)(int64_t, int32_t,
                                                               int32_t, int32_t,
                                                               int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32Int32Int32Int32)(
    int64_t, int32_t, int32_t, int32_t, int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32Int32Int32General)(
    int64_t, int32_t, int32_t, int32_t, int32_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32Int32General)(
    int64_t, int32_t, int32_t, int32_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32General)(int64_t, int32_t,
                                                            int32_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int32Int64Int32)(int64_t, int32_t,
                                                               int32_t, int64_t,
                                                               int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32GeneralInt32)(int64_t, int32_t,
                                                            int64_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32GeneralInt32Int32)(
    int64_t, int32_t, int64_t, int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt32Int64Int64Int32)(int64_t, int32_t,
                                                               int64_t, int64_t,
                                                               int32_t);
typedef int32_t (*Prototype_Int32_GeneralGeneral)(int64_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralGeneralGeneral)(int64_t, int64_t,
                                                         int64_t);
typedef int32_t (*Prototype_Int32_GeneralGeneralInt32Int32)(int64_t, int64_t,
                                                            int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int32Int32)(int64_t, int64_t,
                                                          int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int32Int32Int32Int32)(
    int64_t, int64_t, int32_t, int32_t, int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int32Int64Int32)(int64_t, int64_t,
                                                               int32_t, int64_t,
                                                               int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int32Int32Int32)(int64_t, int64_t,
                                                               int32_t, int32_t,
                                                               int32_t);
typedef int32_t (*Prototype_Int32_GeneralGeneralInt32Int32Int32GeneralInt32)(
    int64_t, int64_t, int32_t, int32_t, int32_t, int64_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralGeneralInt32General)(int32_t, int32_t,
                                                              int32_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int32Int64General)(
    int64_t, int64_t, int32_t, int64_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int64Int64)(int64_t, int64_t,
                                                          int64_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int64Int64Int32)(int64_t, int64_t,
                                                               int64_t, int64_t,
                                                               int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int64General)(int64_t, int64_t,
                                                            int64_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int64Int64General)(
    int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int64Int64Int32Int32)(
    int64_t, int64_t, int64_t, int64_t, int32_t, int32_t);
typedef int64_t (*Prototype_General_GeneralInt32)(int64_t, int32_t);
typedef int64_t (*Prototype_General_GeneralInt32Int32)(int64_t, int32_t,
                                                       int32_t);
typedef int64_t (*Prototype_General_GeneralInt32General)(int64_t, int32_t,
                                                         int64_t);
typedef int64_t (*Prototype_General_GeneralInt32Int32GeneralInt32)(
    int64_t, int32_t, int32_t, int64_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralGeneralInt32GeneralInt32Int32Int32)(
    int64_t, int64_t, int32_t, int64_t, int32_t, int32_t, int32_t);
typedef int64_t (*Prototype_Int64_General)(int64_t);
typedef int64_t (*Prototype_Int64_GeneralInt32)(int64_t, int32_t);
typedef int64_t (*Prototype_Int64_GeneralInt64)(int64_t, int64_t);
typedef int64_t (*Prototype_Int64_GeneralInt64Int32)(int64_t, int64_t, int32_t);
typedef int32_t (*Prototype_Int32_GeneralInt64Int64General)(int64_t, int64_t,
                                                            int64_t, int64_t);
// Generated by Assembler::break_()/stop(), ebreak code is passed as immediate
// field of a subsequent LUI instruction; otherwise returns -1
static inline uint32_t get_ebreak_code(Instruction* instr) {
  MOZ_ASSERT(instr->InstructionBits() == kBreakInstr);
  uint8_t* cur = reinterpret_cast<uint8_t*>(instr);
  Instruction* next_instr = reinterpret_cast<Instruction*>(cur + kInstrSize);
  if (next_instr->BaseOpcodeFieldRaw() == LUI)
    return (next_instr->Imm20UValue());
  else
    return -1;
}

// Software interrupt instructions are used by the simulator to call into C++.
void Simulator::SoftwareInterrupt() {
  // There are two instructions that could get us here, the ebreak or ecall
  // instructions are "SYSTEM" class opcode distinuished by Imm12Value field w/
  // the rest of instruction fields being zero
  // We first check if we met a call_rt_redirected.
  if (instr_.InstructionBits() == rtCallRedirInstr) {
    Redirection* redirection = Redirection::FromSwiInstruction(instr_.instr());
    uintptr_t nativeFn =
        reinterpret_cast<uintptr_t>(redirection->nativeFunction());

    intptr_t arg0 = getRegister(a0);
    intptr_t arg1 = getRegister(a1);
    intptr_t arg2 = getRegister(a2);
    intptr_t arg3 = getRegister(a3);
    intptr_t arg4 = getRegister(a4);
    intptr_t arg5 = getRegister(a5);
    intptr_t arg6 = getRegister(a6);
    intptr_t arg7 = getRegister(a7);

    // This is dodgy but it works because the C entry stubs are never moved.
    // See comment in codegen-arm.cc and bug 1242173.
    intptr_t saved_ra = getRegister(ra);

    intptr_t external =
        reinterpret_cast<intptr_t>(redirection->nativeFunction());

    bool stack_aligned = (getRegister(sp) & (ABIStackAlignment - 1)) == 0;
    if (!stack_aligned) {
      fprintf(stderr, "Runtime call with unaligned stack!\n");
      MOZ_CRASH();
    }

    if (single_stepping_) {
      single_step_callback_(single_step_callback_arg_, this, nullptr);
    }
    if (FLAG_trace_sim) {
      printf(
          "Call to host function at %p with args %ld, %ld, %ld, %ld, %ld, %ld, "
          "%ld, %ld\n",
          reinterpret_cast<void*>(external), arg0, arg1, arg2, arg3, arg4, arg5,
          arg6, arg7);
    }
    switch (redirection->type()) {
      case Args_General0: {
        Prototype_General0 target =
            reinterpret_cast<Prototype_General0>(external);
        int64_t result = target();
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General1: {
        Prototype_General1 target =
            reinterpret_cast<Prototype_General1>(external);
        int64_t result = target(arg0);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General2: {
        Prototype_General2 target =
            reinterpret_cast<Prototype_General2>(external);
        int64_t result = target(arg0, arg1);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General3: {
        Prototype_General3 target =
            reinterpret_cast<Prototype_General3>(external);
        int64_t result = target(arg0, arg1, arg2);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        if (external == intptr_t(&js::wasm::Instance::wake_m32)) {
          result = int32_t(result);
        }
        setCallResult(result);
        break;
      }
      case Args_General4: {
        Prototype_General4 target =
            reinterpret_cast<Prototype_General4>(external);
        int64_t result = target(arg0, arg1, arg2, arg3);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General5: {
        Prototype_General5 target =
            reinterpret_cast<Prototype_General5>(external);
        int64_t result = target(arg0, arg1, arg2, arg3, arg4);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General6: {
        Prototype_General6 target =
            reinterpret_cast<Prototype_General6>(external);
        int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General7: {
        Prototype_General7 target =
            reinterpret_cast<Prototype_General7>(external);
        int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5, arg6);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_General8: {
        Prototype_General8 target =
            reinterpret_cast<Prototype_General8>(external);
        int64_t result = target(arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setCallResult(result);
        break;
      }
      case Args_Double_None: {
        Prototype_Double_None target =
            reinterpret_cast<Prototype_Double_None>(external);
        double dresult = target();
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Int_Double: {
        double dval0 = getFpuRegisterDouble(fa0);
        Prototype_Int_Double target =
            reinterpret_cast<Prototype_Int_Double>(external);
        int64_t result = target(dval0);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        if (external == intptr_t((int32_t(*)(double))JS::ToInt32)) {
          result = int32_t(result);
        }
        setRegister(a0, result);
        break;
      }
      case Args_Int_GeneralGeneralGeneralInt64: {
        Prototype_GeneralGeneralGeneralInt64 target =
            reinterpret_cast<Prototype_GeneralGeneralGeneralInt64>(external);
        int64_t result = target(arg0, arg1, arg2, arg3);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        if (external == intptr_t(&js::wasm::Instance::wait_i32_m32)) {
          result = int32_t(result);
        }
        setRegister(a0, result);
        break;
      }
      case Args_Int_GeneralGeneralInt64Int64: {
        Prototype_GeneralGeneralInt64Int64 target =
            reinterpret_cast<Prototype_GeneralGeneralInt64Int64>(external);
        int64_t result = target(arg0, arg1, arg2, arg3);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        if (external == intptr_t(&js::wasm::Instance::wait_i64_m32)) {
          result = int32_t(result);
        }
        setRegister(a0, result);
        break;
      }
      case Args_Int_DoubleInt: {
        double dval = getFpuRegisterDouble(fa0);
        Prototype_Int_DoubleInt target =
            reinterpret_cast<Prototype_Int_DoubleInt>(external);
        int64_t result = target(dval, arg0);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setRegister(a0, result);
        break;
      }
      case Args_Int_DoubleIntInt: {
        double dval = getFpuRegisterDouble(fa0);
        Prototype_Int_DoubleIntInt target =
            reinterpret_cast<Prototype_Int_DoubleIntInt>(external);
        int64_t result = target(dval, arg1, arg2);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setRegister(a0, result);
        break;
      }
      case Args_Int_IntDoubleIntInt: {
        double dval = getFpuRegisterDouble(fa0);
        Prototype_Int_IntDoubleIntInt target =
            reinterpret_cast<Prototype_Int_IntDoubleIntInt>(external);
        int64_t result = target(arg0, dval, arg2, arg3);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setRegister(a0, result);
        break;
      }
      case Args_Double_Double: {
        double dval0 = getFpuRegisterDouble(fa0);
        Prototype_Double_Double target =
            reinterpret_cast<Prototype_Double_Double>(external);
        double dresult = target(dval0);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Float32_Float32: {
        float fval0;
        fval0 = getFpuRegisterFloat(fa0);
        Prototype_Float32_Float32 target =
            reinterpret_cast<Prototype_Float32_Float32>(external);
        float fresult = target(fval0);
        if (FLAG_trace_sim) printf("ret %f\n", fresult);
        setCallResultFloat(fresult);
        break;
      }
      case Args_Int_Float32: {
        float fval0;
        fval0 = getFpuRegisterFloat(fa0);
        Prototype_Int_Float32 target =
            reinterpret_cast<Prototype_Int_Float32>(external);
        int64_t result = target(fval0);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setRegister(a0, result);
        break;
      }
      case Args_Float32_Float32Float32: {
        float fval0;
        float fval1;
        fval0 = getFpuRegisterFloat(fa0);
        fval1 = getFpuRegisterFloat(fa1);
        Prototype_Float32_Float32Float32 target =
            reinterpret_cast<Prototype_Float32_Float32Float32>(external);
        float fresult = target(fval0, fval1);
        if (FLAG_trace_sim) printf("ret %f\n", fresult);
        setCallResultFloat(fresult);
        break;
      }
      case Args_Double_Int: {
        Prototype_Double_Int target =
            reinterpret_cast<Prototype_Double_Int>(external);
        double dresult = target(arg0);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Double_DoubleInt: {
        double dval0 = getFpuRegisterDouble(fa0);
        Prototype_Double_DoubleInt target =
            reinterpret_cast<Prototype_Double_DoubleInt>(external);
        double dresult = target(dval0, arg0);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Double_DoubleDouble: {
        double dval0 = getFpuRegisterDouble(fa0);
        double dval1 = getFpuRegisterDouble(fa1);
        Prototype_Double_DoubleDouble target =
            reinterpret_cast<Prototype_Double_DoubleDouble>(external);
        double dresult = target(dval0, dval1);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Double_IntDouble: {
        double dval0 = getFpuRegisterDouble(fa0);
        Prototype_Double_IntDouble target =
            reinterpret_cast<Prototype_Double_IntDouble>(external);
        double dresult = target(arg0, dval0);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Int_IntDouble: {
        double dval0 = getFpuRegisterDouble(fa0);
        Prototype_Int_IntDouble target =
            reinterpret_cast<Prototype_Int_IntDouble>(external);
        int64_t result = target(arg0, dval0);
        if (FLAG_trace_sim) printf("ret %ld\n", result);
        setRegister(a0, result);
        break;
      }
      case Args_Double_DoubleDoubleDouble: {
        double dval0 = getFpuRegisterDouble(fa0);
        double dval1 = getFpuRegisterDouble(fa1);
        double dval2 = getFpuRegisterDouble(fa2);
        Prototype_Double_DoubleDoubleDouble target =
            reinterpret_cast<Prototype_Double_DoubleDoubleDouble>(external);
        double dresult = target(dval0, dval1, dval2);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Double_DoubleDoubleDoubleDouble: {
        double dval0 = getFpuRegisterDouble(fa0);
        double dval1 = getFpuRegisterDouble(fa1);
        double dval2 = getFpuRegisterDouble(fa2);
        double dval3 = getFpuRegisterDouble(fa3);
        Prototype_Double_DoubleDoubleDoubleDouble target =
            reinterpret_cast<Prototype_Double_DoubleDoubleDoubleDouble>(
                external);
        double dresult = target(dval0, dval1, dval2, dval3);
        if (FLAG_trace_sim) printf("ret %f\n", dresult);
        setCallResultDouble(dresult);
        break;
      }
      case Args_Int32_General: {
        int32_t ret = reinterpret_cast<Prototype_Int32_General>(nativeFn)(arg0);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralInt32>(nativeFn)(
            arg0, I32(arg1));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralInt32Int32>(
            nativeFn)(arg0, I32(arg1), I32(arg2));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32Int32: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralInt32Int32Int32>(
            nativeFn)(arg0, I32(arg1), I32(arg2), I32(arg3));
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32Int32Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32Int32Int32Int32>(
                nativeFn)(arg0, I32(arg1), I32(arg2), I32(arg3), I32(arg4));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32Int32Int32Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32Int32Int32Int32Int32>(
                nativeFn)(arg0, I32(arg1), I32(arg2), I32(arg3), I32(arg4),
                          I32(arg5));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32Int32Int32General: {
        int32_t ret = reinterpret_cast<
            Prototype_Int32_GeneralInt32Int32Int32Int32General>(nativeFn)(
            arg0, I32(arg1), I32(arg2), I32(arg3), I32(arg4), arg5);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32Int32General: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32Int32Int32General>(
                nativeFn)(arg0, I32(arg1), I32(arg2), I32(arg3), arg4);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32General: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32Int32General>(
                nativeFn)(arg0, I32(arg1), I32(arg2), arg3);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int32Int64Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32Int32Int64Int32>(
                nativeFn)(arg0, I32(arg1), I32(arg2), arg3, I32(arg4));
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32GeneralInt32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32GeneralInt32>(
                nativeFn)(arg0, I32(arg1), arg2, I32(arg3));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32GeneralInt32Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32GeneralInt32Int32>(
                nativeFn)(arg0, I32(arg1), arg2, I32(arg3), I32(arg4));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralGeneral: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralGeneral>(
            nativeFn)(arg0, arg1);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralInt32Int64Int64Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt32Int64Int64Int32>(
                nativeFn)(arg0, I32(arg1), arg2, arg3, I32(arg4));
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralGeneralGeneral: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralGeneralGeneral>(
            nativeFn)(arg0, arg1, arg2);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_Int32_GeneralGeneralInt32Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralGeneralInt32Int32>(
                nativeFn)(arg0, arg1, I32(arg2), I32(arg3));
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int32Int32: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralInt64Int32Int32>(
            nativeFn)(arg0, arg1, I32(arg2), I32(arg3));
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int32Int32Int32Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int32Int32Int32Int32>(
                nativeFn)(arg0, arg1, I32(arg2), I32(arg3), I32(arg4),
                          I32(arg5));
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int32Int64Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int32Int64Int32>(
                nativeFn)(arg0, arg1, I32(arg2), arg3, I32(arg4));
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int32Int64General: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int32Int64General>(
                nativeFn)(arg0, arg1, I32(arg2), arg3, arg4);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int64Int64: {
        int32_t ret = reinterpret_cast<Prototype_Int32_GeneralInt64Int64Int64>(
            nativeFn)(arg0, arg1, arg2, arg3);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int64Int64Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int64Int64Int32>(
                nativeFn)(arg0, arg1, arg2, arg3, I32(arg4));
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int64General: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int64General>(
                nativeFn)(arg0, arg1, arg2, arg3);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int64Int64General: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int64Int64General>(
                nativeFn)(arg0, arg1, arg2, arg3, arg4);
        if (FLAG_trace_sim) printf("ret %d\n", ret);
        setRegister(a0, I64(ret));
        break;
      }
      case Args_General_GeneralInt32: {
        int64_t ret = reinterpret_cast<Prototype_General_GeneralInt32>(
            nativeFn)(arg0, I32(arg1));
        if (FLAG_trace_sim) printf("ret %ld\n", ret);
        setRegister(a0, ret);
        break;
      }
      case js::jit::Args_Int32_GeneralInt64Int64Int64Int32Int32: {
        int32_t ret =
            reinterpret_cast<Prototype_Int32_GeneralInt64Int64Int64Int32Int32>(
                nativeFn)(arg0, arg1, arg2, arg3, I32(arg4), I32(arg5));
        setRegister(a0, I64(ret));
        break;
      }
      case Args_General_GeneralInt32Int32: {
        int64_t ret = reinterpret_cast<Prototype_General_GeneralInt32Int32>(
            nativeFn)(arg0, I32(arg1), I32(arg2));
        if (FLAG_trace_sim) printf("ret %ld\n", ret);
        setRegister(a0, ret);
        break;
      }
      case Args_General_GeneralInt32General: {
        int64_t ret = reinterpret_cast<Prototype_General_GeneralInt32General>(
            nativeFn)(arg0, I32(arg1), arg2);
        if (FLAG_trace_sim) printf("ret %ld\n", ret);
        setRegister(a0, ret);
        break;
      }
      case js::jit::Args_General_GeneralInt32Int32GeneralInt32: {
        int64_t ret =
            reinterpret_cast<Prototype_General_GeneralInt32Int32GeneralInt32>(
                nativeFn)(arg0, I32(arg1), I32(arg2), arg3, I32(arg4));
        setRegister(a0, ret);
        break;
      }
      case js::jit::Args_Int32_GeneralGeneralInt32Int32Int32GeneralInt32: {
        int32_t ret = reinterpret_cast<
            Prototype_Int32_GeneralGeneralInt32Int32Int32GeneralInt32>(
            nativeFn)(arg0, arg1, I32(arg2), I32(arg3), I32(arg4), arg5,
                      I32(arg6));
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int32_GeneralGeneralInt32General: {
        Prototype_Int32_GeneralGeneralInt32General target =
            reinterpret_cast<Prototype_Int32_GeneralGeneralInt32General>(
                external);
        int64_t result = target(I32(arg0), I32(arg1), I32(arg2), I32(arg3));
        setRegister(a0, I64(result));
        break;
      }
      case js::jit::Args_Int32_GeneralGeneralInt32GeneralInt32Int32Int32: {
        int64_t arg6 = getRegister(a6);
        int32_t ret = reinterpret_cast<
            Prototype_Int32_GeneralGeneralInt32GeneralInt32Int32Int32>(
            nativeFn)(arg0, arg1, I32(arg2), arg3, I32(arg4), I32(arg5),
                      I32(arg6));
        setRegister(a0, I64(ret));
        break;
      }
      case js::jit::Args_Int64_General: {
        int64_t ret = reinterpret_cast<Prototype_Int64_General>(nativeFn)(arg0);
        if (FLAG_trace_sim) printf("ret %ld\n", ret);
        setRegister(a0, ret);
        break;
      }
      case js::jit::Args_Int64_GeneralInt32: {
        int64_t ret = reinterpret_cast<Prototype_Int64_GeneralInt32>(nativeFn)(
            arg0, I32(arg1));
        setRegister(a0, ret);
        break;
      }
      case js::jit::Args_Int64_GeneralInt64: {
        int64_t ret = reinterpret_cast<Prototype_Int64_GeneralInt64>(nativeFn)(
            arg0, arg1);
        setRegister(a0, ret);
        break;
      }
      case js::jit::Args_Int64_GeneralInt64Int32: {
        int64_t ret = reinterpret_cast<Prototype_Int64_GeneralInt64Int32>(
            nativeFn)(arg0, arg1, I32(arg2));
        setRegister(a0, ret);
        break;
      }
      default:
        MOZ_CRASH("Unknown function type.");
    }

    if (single_stepping_) {
      single_step_callback_(single_step_callback_arg_, this, nullptr);
    }

    setRegister(ra, saved_ra);
    set_pc(getRegister(ra));

  } else if (instr_.InstructionBits() == kBreakInstr &&
             (get_ebreak_code(instr_.instr()) <= kMaxStopCode)) {
    uint32_t code = get_ebreak_code(instr_.instr());
    if (isWatchpoint(code)) {
      printWatchpoint(code);
    } else if (IsTracepoint(code)) {
      if (!FLAG_debug_sim) {
        MOZ_CRASH("Add --debug-sim when tracepoint instruction is used.\n");
      }
      // printf("%d %d %d %d %d %d %d\n", code, code & LOG_TRACE, code &
      // LOG_REGS,
      //        code & kDebuggerTracingDirectivesMask, TRACE_ENABLE,
      //        TRACE_DISABLE, kDebuggerTracingDirectivesMask);
      switch (code & kDebuggerTracingDirectivesMask) {
        case TRACE_ENABLE:
          if (code & LOG_TRACE) {
            FLAG_trace_sim = true;
          }
          if (code & LOG_REGS) {
            RiscvDebugger dbg(this);
            dbg.printAllRegs();
          }
          break;
        case TRACE_DISABLE:
          if (code & LOG_TRACE) {
            FLAG_trace_sim = false;
          }
          break;
        default:
          UNREACHABLE();
      }
    } else {
      increaseStopCounter(code);
      handleStop(code);
    }
  } else {
    //     uint8_t code = get_ebreak_code(instr_.instr()) - kMaxStopCode - 1;
    //     switch (LNode::Opcode(code)) {
    // #define EMIT_OP(OP, ...)  \
//       case LNode::Opcode::OP:\
//            std::cout << #OP << std::endl; \
//            break;
    //     LIR_OPCODE_LIST(EMIT_OP);
    // #undef EMIT_OP
    //     }
    DieOrDebug();
  }
}

// Stop helper functions.
bool Simulator::isWatchpoint(uint32_t code) {
  return (code <= kMaxWatchpointCode);
}

bool Simulator::IsTracepoint(uint32_t code) {
  return (code <= kMaxTracepointCode && code > kMaxWatchpointCode);
}

void Simulator::printWatchpoint(uint32_t code) {
  RiscvDebugger dbg(this);
  ++break_count_;
  if (FLAG_riscv_print_watchpoint) {
    printf("\n---- break %d marker: %20" PRIi64 "  (instr count: %20" PRIi64
           ") ----\n",
           code, break_count_, icount_);
    dbg.printAllRegs();  // Print registers and continue running.
  }
}

void Simulator::handleStop(uint32_t code) {
  // Stop if it is enabled, otherwise go on jumping over the stop
  // and the message address.
  if (isEnabledStop(code)) {
    RiscvDebugger dbg(this);
    dbg.Debug();
  } else {
    set_pc(get_pc() + 2 * kInstrSize);
  }
}

bool Simulator::isStopInstruction(SimInstruction* instr) {
  if (instr->InstructionBits() != kBreakInstr) return false;
  int32_t code = get_ebreak_code(instr->instr());
  return code != -1 && static_cast<uint32_t>(code) > kMaxWatchpointCode &&
         static_cast<uint32_t>(code) <= kMaxStopCode;
}

bool Simulator::isEnabledStop(uint32_t code) {
  MOZ_ASSERT(code <= kMaxStopCode);
  MOZ_ASSERT(code > kMaxWatchpointCode);
  return !(watchedStops_[code].count_ & kStopDisabledBit);
}

void Simulator::enableStop(uint32_t code) {
  if (!isEnabledStop(code)) {
    watchedStops_[code].count_ &= ~kStopDisabledBit;
  }
}

void Simulator::disableStop(uint32_t code) {
  if (isEnabledStop(code)) {
    watchedStops_[code].count_ |= kStopDisabledBit;
  }
}

void Simulator::increaseStopCounter(uint32_t code) {
  MOZ_ASSERT(code <= kMaxStopCode);
  if ((watchedStops_[code].count_ & ~(1 << 31)) == 0x7fffffff) {
    printf(
        "Stop counter for code %i has overflowed.\n"
        "Enabling this code and reseting the counter to 0.\n",
        code);
    watchedStops_[code].count_ = 0;
    enableStop(code);
  } else {
    watchedStops_[code].count_++;
  }
}

// Print a stop status.
void Simulator::printStopInfo(uint32_t code) {
  if (code <= kMaxWatchpointCode) {
    printf("That is a watchpoint, not a stop.\n");
    return;
  } else if (code > kMaxStopCode) {
    printf("Code too large, only %u stops can be used\n", kMaxStopCode + 1);
    return;
  }
  const char* state = isEnabledStop(code) ? "Enabled" : "Disabled";
  int32_t count = watchedStops_[code].count_ & ~kStopDisabledBit;
  // Don't print the state of unused breakpoints.
  if (count != 0) {
    if (watchedStops_[code].desc_) {
      printf("stop %i - 0x%x: \t%s, \tcounter = %i, \t%s\n", code, code, state,
             count, watchedStops_[code].desc_);
    } else {
      printf("stop %i - 0x%x: \t%s, \tcounter = %i\n", code, code, state,
             count);
    }
  }
}

void Simulator::SignalException(Exception e) {
  printf("Error: Exception %i raised.", static_cast<int>(e));
  MOZ_CRASH();
}

// TODO(plind): refactor this messy debug code when we do unaligned access.
void Simulator::DieOrDebug() {
  if (FLAG_riscv_trap_to_simulator_debugger) {
    RiscvDebugger dbg(this);
    dbg.Debug();
  } else {
    MOZ_CRASH("Die");
  }
}

// Executes the current instruction.
void Simulator::InstructionDecode(Instruction* instr) {
  // if (FLAG_check_icache) {
  //   CheckICache(SimulatorProcess::icache(), instr);
  // }
  pc_modified_ = false;

  EmbeddedVector<char, 256> buffer;

  if (FLAG_trace_sim || FLAG_debug_sim) {
    SNPrintF(trace_buf_, " ");
    disasm::NameConverter converter;
    disasm::Disassembler dasm(converter);
    // Use a reasonably large buffer.
    dasm.InstructionDecode(buffer, reinterpret_cast<byte*>(instr));

    // printf("EXECUTING  0x%08" PRIxPTR "   %-44s\n",
    //        reinterpret_cast<intptr_t>(instr), buffer.begin());
  }

  instr_ = instr;
  switch (instr_.InstructionType()) {
    case Instruction::kRType:
      DecodeRVRType();
      break;
    case Instruction::kR4Type:
      DecodeRVR4Type();
      break;
    case Instruction::kIType:
      DecodeRVIType();
      break;
    case Instruction::kSType:
      DecodeRVSType();
      break;
    case Instruction::kBType:
      DecodeRVBType();
      break;
    case Instruction::kUType:
      DecodeRVUType();
      break;
    case Instruction::kJType:
      DecodeRVJType();
      break;
    case Instruction::kCRType:
      DecodeCRType();
      break;
    case Instruction::kCAType:
      DecodeCAType();
      break;
    case Instruction::kCJType:
      DecodeCJType();
      break;
    case Instruction::kCBType:
      DecodeCBType();
      break;
    case Instruction::kCIType:
      DecodeCIType();
      break;
    case Instruction::kCIWType:
      DecodeCIWType();
      break;
    case Instruction::kCSSType:
      DecodeCSSType();
      break;
    case Instruction::kCLType:
      DecodeCLType();
      break;
    case Instruction::kCSType:
      DecodeCSType();
      break;
#  ifdef CAN_USE_RVV_INSTRUCTIONS
    case Instruction::kVType:
      DecodeVType();
      break;
#  endif
    default:
      UNSUPPORTED();
  }

  if (FLAG_trace_sim) {
    printf("  0x%012" PRIxPTR "      %-44s\t%s\n",
           reinterpret_cast<intptr_t>(instr), buffer.start(),
           trace_buf_.start());
  }

  if (!pc_modified_) {
    setRegister(pc, reinterpret_cast<sreg_t>(instr) + instr->InstructionSize());
  }

  if (watch_address_ != nullptr) {
    printf("  0x%012" PRIxPTR " :  0x%016" REGIx_FORMAT "  %14" REGId_FORMAT
           " \n",
           reinterpret_cast<intptr_t>(watch_address_), *watch_address_,
           *watch_address_);
    if (watch_value_ != *watch_address_) {
      RiscvDebugger dbg(this);
      dbg.Debug();
      watch_value_ = *watch_address_;
    }
  }
}

void Simulator::enable_single_stepping(SingleStepCallback cb, void* arg) {
  single_stepping_ = true;
  single_step_callback_ = cb;
  single_step_callback_arg_ = arg;
  single_step_callback_(single_step_callback_arg_, this, (void*)get_pc());
}

void Simulator::disable_single_stepping() {
  if (!single_stepping_) {
    return;
  }
  single_step_callback_(single_step_callback_arg_, this, (void*)get_pc());
  single_stepping_ = false;
  single_step_callback_ = nullptr;
  single_step_callback_arg_ = nullptr;
}

template <bool enableStopSimAt>
void Simulator::execute() {
  if (single_stepping_) {
    single_step_callback_(single_step_callback_arg_, this, nullptr);
  }

  // Get the PC to simulate. Cannot use the accessor here as we need the
  // raw PC value and not the one used as input to arithmetic instructions.
  int64_t program_counter = get_pc();

  while (program_counter != end_sim_pc) {
    if (enableStopSimAt && (icount_ == Simulator::StopSimAt)) {
      RiscvDebugger dbg(this);
      dbg.Debug();
    }
    if (single_stepping_) {
      single_step_callback_(single_step_callback_arg_, this,
                            (void*)program_counter);
    }
    Instruction* instr = reinterpret_cast<Instruction*>(program_counter);
    InstructionDecode(instr);
    icount_++;
    program_counter = get_pc();
  }

  if (single_stepping_) {
    single_step_callback_(single_step_callback_arg_, this, nullptr);
  }
}

// RISCV Instruction Decode Routine
void Simulator::DecodeRVRType() {
  switch (instr_.InstructionBits() & kRTypeMask) {
    case RO_ADD: {
      set_rd(sext_xlen(rs1() + rs2()));
      break;
    }
    case RO_SUB: {
      set_rd(sext_xlen(rs1() - rs2()));
      break;
    }
    case RO_SLL: {
      set_rd(sext_xlen(rs1() << (rs2() & (xlen - 1))));
      break;
    }
    case RO_SLT: {
      set_rd(sreg_t(rs1()) < sreg_t(rs2()));
      break;
    }
    case RO_SLTU: {
      set_rd(reg_t(rs1()) < reg_t(rs2()));
      break;
    }
    case RO_XOR: {
      set_rd(rs1() ^ rs2());
      break;
    }
    case RO_SRL: {
      set_rd(sext_xlen(zext_xlen(rs1()) >> (rs2() & (xlen - 1))));
      break;
    }
    case RO_SRA: {
      set_rd(sext_xlen(sext_xlen(rs1()) >> (rs2() & (xlen - 1))));
      break;
    }
    case RO_OR: {
      set_rd(rs1() | rs2());
      break;
    }
    case RO_AND: {
      set_rd(rs1() & rs2());
      break;
    }
#  ifdef JS_CODEGEN_RISCV64
    case RO_ADDW: {
      set_rd(sext32(rs1() + rs2()));
      break;
    }
    case RO_SUBW: {
      set_rd(sext32(rs1() - rs2()));
      break;
    }
    case RO_SLLW: {
      set_rd(sext32(rs1() << (rs2() & 0x1F)));
      break;
    }
    case RO_SRLW: {
      set_rd(sext32(uint32_t(rs1()) >> (rs2() & 0x1F)));
      break;
    }
    case RO_SRAW: {
      set_rd(sext32(int32_t(rs1()) >> (rs2() & 0x1F)));
      break;
    }
#  endif /* JS_CODEGEN_RISCV64 */
      // TODO(riscv): Add RISCV M extension macro
    case RO_MUL: {
      set_rd(rs1() * rs2());
      break;
    }
    case RO_MULH: {
      set_rd(mulh(rs1(), rs2()));
      break;
    }
    case RO_MULHSU: {
      set_rd(mulhsu(rs1(), rs2()));
      break;
    }
    case RO_MULHU: {
      set_rd(mulhu(rs1(), rs2()));
      break;
    }
    case RO_DIV: {
      sreg_t lhs = sext_xlen(rs1());
      sreg_t rhs = sext_xlen(rs2());
      if (rhs == 0) {
        set_rd(-1);
      } else if (lhs == INTPTR_MIN && rhs == -1) {
        set_rd(lhs);
      } else {
        set_rd(sext_xlen(lhs / rhs));
      }
      break;
    }
    case RO_DIVU: {
      reg_t lhs = zext_xlen(rs1());
      reg_t rhs = zext_xlen(rs2());
      if (rhs == 0) {
        set_rd(UINTPTR_MAX);
      } else {
        set_rd(zext_xlen(lhs / rhs));
      }
      break;
    }
    case RO_REM: {
      sreg_t lhs = sext_xlen(rs1());
      sreg_t rhs = sext_xlen(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else if (lhs == INTPTR_MIN && rhs == -1) {
        set_rd(0);
      } else {
        set_rd(sext_xlen(lhs % rhs));
      }
      break;
    }
    case RO_REMU: {
      reg_t lhs = zext_xlen(rs1());
      reg_t rhs = zext_xlen(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else {
        set_rd(zext_xlen(lhs % rhs));
      }
      break;
    }
#  ifdef JS_CODEGEN_RISCV64
    case RO_MULW: {
      set_rd(sext32(sext32(rs1()) * sext32(rs2())));
      break;
    }
    case RO_DIVW: {
      sreg_t lhs = sext32(rs1());
      sreg_t rhs = sext32(rs2());
      if (rhs == 0) {
        set_rd(-1);
      } else if (lhs == INT32_MIN && rhs == -1) {
        set_rd(lhs);
      } else {
        set_rd(sext32(lhs / rhs));
      }
      break;
    }
    case RO_DIVUW: {
      reg_t lhs = zext32(rs1());
      reg_t rhs = zext32(rs2());
      if (rhs == 0) {
        set_rd(UINT32_MAX);
      } else {
        set_rd(zext32(lhs / rhs));
      }
      break;
    }
    case RO_REMW: {
      sreg_t lhs = sext32(rs1());
      sreg_t rhs = sext32(rs2());
      if (rhs == 0) {
        set_rd(lhs);
      } else if (lhs == INT32_MIN && rhs == -1) {
        set_rd(0);
      } else {
        set_rd(sext32(lhs % rhs));
      }
      break;
    }
    case RO_REMUW: {
      reg_t lhs = zext32(rs1());
      reg_t rhs = zext32(rs2());
      if (rhs == 0) {
        set_rd(zext32(lhs));
      } else {
        set_rd(zext32(lhs % rhs));
      }
      break;
    }
#  endif /*JS_CODEGEN_RISCV64*/
      // TODO(riscv): End Add RISCV M extension macro
    default: {
      switch (instr_.BaseOpcode()) {
        case AMO:
          DecodeRVRAType();
          break;
        case OP_FP:
          DecodeRVRFPType();
          break;
        default:
          UNSUPPORTED();
      }
    }
  }
}

template <typename T>
T Simulator::FMaxMinHelper(T a, T b, MaxMinKind kind) {
  // set invalid bit for signaling nan
  if ((a == std::numeric_limits<T>::signaling_NaN()) ||
      (b == std::numeric_limits<T>::signaling_NaN())) {
    set_csr_bits(csr_fflags, kInvalidOperation);
  }

  T result = 0;
  if (std::isnan(a) && std::isnan(b)) {
    result = std::numeric_limits<float>::quiet_NaN();
  } else if (std::isnan(a)) {
    result = b;
  } else if (std::isnan(b)) {
    result = a;
  } else if (b == a) {  // Handle -0.0 == 0.0 case.
    if (kind == MaxMinKind::kMax) {
      result = std::signbit(b) ? a : b;
    } else {
      result = std::signbit(b) ? b : a;
    }
  } else {
    result = (kind == MaxMinKind::kMax) ? fmax(a, b) : fmin(a, b);
  }

  return result;
}

float Simulator::RoundF2FHelper(float input_val, int rmode) {
  if (rmode == DYN) rmode = get_dynamic_rounding_mode();

  float rounded = 0;
  switch (rmode) {
    case RNE: {  // Round to Nearest, tiest to Even
      rounded = floorf(input_val);
      float error = input_val - rounded;

      // Take care of correctly handling the range [-0.5, -0.0], which must
      // yield -0.0.
      if ((-0.5 <= input_val) && (input_val < 0.0)) {
        rounded = -0.0;

        // If the error is greater than 0.5, or is equal to 0.5 and the integer
        // result is odd, round up.
      } else if ((error > 0.5) ||
                 ((error == 0.5) && (std::fmod(rounded, 2) != 0))) {
        rounded++;
      }
      break;
    }
    case RTZ:  // Round towards Zero
      rounded = std::truncf(input_val);
      break;
    case RDN:  // Round Down (towards -infinity)
      rounded = floorf(input_val);
      break;
    case RUP:  // Round Up (towards +infinity)
      rounded = ceilf(input_val);
      break;
    case RMM:  // Round to Nearest, tiest to Max Magnitude
      rounded = std::roundf(input_val);
      break;
    default:
      UNREACHABLE();
  }

  return rounded;
}

double Simulator::RoundF2FHelper(double input_val, int rmode) {
  if (rmode == DYN) rmode = get_dynamic_rounding_mode();

  double rounded = 0;
  switch (rmode) {
    case RNE: {  // Round to Nearest, tiest to Even
      rounded = std::floor(input_val);
      double error = input_val - rounded;

      // Take care of correctly handling the range [-0.5, -0.0], which must
      // yield -0.0.
      if ((-0.5 <= input_val) && (input_val < 0.0)) {
        rounded = -0.0;

        // If the error is greater than 0.5, or is equal to 0.5 and the integer
        // result is odd, round up.
      } else if ((error > 0.5) ||
                 ((error == 0.5) && (std::fmod(rounded, 2) != 0))) {
        rounded++;
      }
      break;
    }
    case RTZ:  // Round towards Zero
      rounded = std::trunc(input_val);
      break;
    case RDN:  // Round Down (towards -infinity)
      rounded = std::floor(input_val);
      break;
    case RUP:  // Round Up (towards +infinity)
      rounded = std::ceil(input_val);
      break;
    case RMM:  // Round to Nearest, tiest to Max Magnitude
      rounded = std::round(input_val);
      break;
    default:
      UNREACHABLE();
  }
  return rounded;
}

// convert rounded floating-point to integer types, handle input values that
// are out-of-range, underflow, or NaN, and set appropriate fflags
template <typename I_TYPE, typename F_TYPE>
I_TYPE Simulator::RoundF2IHelper(F_TYPE original, int rmode) {
  MOZ_ASSERT(std::is_integral<I_TYPE>::value);

  MOZ_ASSERT((std::is_same<F_TYPE, float>::value ||
              std::is_same<F_TYPE, double>::value));

  I_TYPE max_i = std::numeric_limits<I_TYPE>::max();
  I_TYPE min_i = std::numeric_limits<I_TYPE>::min();

  if (!std::isfinite(original)) {
    set_fflags(kInvalidOperation);
    if (std::isnan(original) ||
        original == std::numeric_limits<F_TYPE>::infinity()) {
      return max_i;
    } else {
      MOZ_ASSERT(original == -std::numeric_limits<F_TYPE>::infinity());
      return min_i;
    }
  }

  F_TYPE rounded = RoundF2FHelper(original, rmode);
  if (original != rounded) set_fflags(kInexact);

  if (!std::isfinite(rounded)) {
    set_fflags(kInvalidOperation);
    if (std::isnan(rounded) ||
        rounded == std::numeric_limits<F_TYPE>::infinity()) {
      return max_i;
    } else {
      MOZ_ASSERT(rounded == -std::numeric_limits<F_TYPE>::infinity());
      return min_i;
    }
  }

  // Since integer max values are either all 1s (for unsigned) or all 1s
  // except for sign-bit (for signed), they cannot be represented precisely in
  // floating point, in order to precisely tell whether the rounded floating
  // point is within the max range, we compare against (max_i+1) which would
  // have a single 1 w/ many trailing zeros
  float max_i_plus_1 =
      std::is_same<uint64_t, I_TYPE>::value
          ? 0x1p64f  // uint64_t::max + 1 cannot be represented in integers,
                     // so use its float representation directly
          : static_cast<float>(static_cast<uint64_t>(max_i) + 1);
  if (rounded >= max_i_plus_1) {
    set_fflags(kOverflow | kInvalidOperation);
    return max_i;
  }

  // Since min_i (either 0 for unsigned, or for signed) is represented
  // precisely in floating-point,  comparing rounded directly against min_i
  if (rounded <= min_i) {
    if (rounded < min_i) set_fflags(kOverflow | kInvalidOperation);
    return min_i;
  }

  F_TYPE underflow_fval =
      std::is_same<F_TYPE, float>::value ? FLT_MIN : DBL_MIN;
  if (rounded < underflow_fval && rounded > -underflow_fval && rounded != 0) {
    set_fflags(kUnderflow);
  }

  return static_cast<I_TYPE>(rounded);
}

template <typename T>
static int64_t FclassHelper(T value) {
  switch (std::fpclassify(value)) {
    case FP_INFINITE:
      return (std::signbit(value) ? kNegativeInfinity : kPositiveInfinity);
    case FP_NAN:
      return (isSnan(value) ? kSignalingNaN : kQuietNaN);
    case FP_NORMAL:
      return (std::signbit(value) ? kNegativeNormalNumber
                                  : kPositiveNormalNumber);
    case FP_SUBNORMAL:
      return (std::signbit(value) ? kNegativeSubnormalNumber
                                  : kPositiveSubnormalNumber);
    case FP_ZERO:
      return (std::signbit(value) ? kNegativeZero : kPositiveZero);
    default:
      UNREACHABLE();
  }
  UNREACHABLE();
  return FP_ZERO;
}

template <typename T>
bool Simulator::CompareFHelper(T input1, T input2, FPUCondition cc) {
  MOZ_ASSERT(std::is_floating_point<T>::value);
  bool result = false;
  switch (cc) {
    case LT:
    case LE:
      // FLT, FLE are signaling compares
      if (std::isnan(input1) || std::isnan(input2)) {
        set_fflags(kInvalidOperation);
        result = false;
      } else {
        result = (cc == LT) ? (input1 < input2) : (input1 <= input2);
      }
      break;
    case EQ:
      if (std::numeric_limits<T>::signaling_NaN() == input1 ||
          std::numeric_limits<T>::signaling_NaN() == input2) {
        set_fflags(kInvalidOperation);
      }
      if (std::isnan(input1) || std::isnan(input2)) {
        result = false;
      } else {
        result = (input1 == input2);
      }
      break;
    case NE:
      if (std::numeric_limits<T>::signaling_NaN() == input1 ||
          std::numeric_limits<T>::signaling_NaN() == input2) {
        set_fflags(kInvalidOperation);
      }
      if (std::isnan(input1) || std::isnan(input2)) {
        result = true;
      } else {
        result = (input1 != input2);
      }
      break;
    default:
      UNREACHABLE();
  }
  return result;
}

template <typename T>
static inline bool is_invalid_fmul(T src1, T src2) {
  return (isinf(src1) && src2 == static_cast<T>(0.0)) ||
         (src1 == static_cast<T>(0.0) && isinf(src2));
}

template <typename T>
static inline bool is_invalid_fadd(T src1, T src2) {
  return (isinf(src1) && isinf(src2) &&
          std::signbit(src1) != std::signbit(src2));
}

template <typename T>
static inline bool is_invalid_fsub(T src1, T src2) {
  return (isinf(src1) && isinf(src2) &&
          std::signbit(src1) == std::signbit(src2));
}

template <typename T>
static inline bool is_invalid_fdiv(T src1, T src2) {
  return ((src1 == 0 && src2 == 0) || (isinf(src1) && isinf(src2)));
}

template <typename T>
static inline bool is_invalid_fsqrt(T src1) {
  return (src1 < 0);
}

int Simulator::loadLinkedW(uint64_t addr, SimInstruction* instr) {
  if ((addr & 3) == 0) {
    if (handleWasmSegFault(addr, 4)) {
      return -1;
    }

    volatile int32_t* ptr = reinterpret_cast<volatile int32_t*>(addr);
    int32_t value = *ptr;
    lastLLValue_ = value;
    LLAddr_ = addr;
    // Note that any memory write or "external" interrupt should reset this
    // value to false.
    LLBit_ = true;
    return value;
  }
  printf("Unaligned write at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

int Simulator::storeConditionalW(uint64_t addr, int value,
                                 SimInstruction* instr) {
  // Correct behavior in this case, as defined by architecture, is to just
  // return 0, but there is no point at allowing that. It is certainly an
  // indicator of a bug.
  if (addr != LLAddr_) {
    printf("SC to bad address: 0x%016" PRIx64 ", pc=0x%016" PRIx64
           ", expected: 0x%016" PRIx64 "\n",
           addr, reinterpret_cast<intptr_t>(instr), LLAddr_);
    MOZ_CRASH();
  }

  if ((addr & 3) == 0) {
    SharedMem<int32_t*> ptr =
        SharedMem<int32_t*>::shared(reinterpret_cast<int32_t*>(addr));

    if (!LLBit_) {
      return 1;
    }

    LLBit_ = false;
    LLAddr_ = 0;
    int32_t expected = int32_t(lastLLValue_);
    int32_t old =
        AtomicOperations::compareExchangeSeqCst(ptr, expected, int32_t(value));
    return (old == expected) ? 0 : 1;
  }
  printf("Unaligned SC at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

int64_t Simulator::loadLinkedD(uint64_t addr, SimInstruction* instr) {
  if ((addr & kPointerAlignmentMask) == 0) {
    if (handleWasmSegFault(addr, 8)) {
      return -1;
    }

    volatile int64_t* ptr = reinterpret_cast<volatile int64_t*>(addr);
    int64_t value = *ptr;
    lastLLValue_ = value;
    LLAddr_ = addr;
    // Note that any memory write or "external" interrupt should reset this
    // value to false.
    LLBit_ = true;
    return value;
  }
  printf("Unaligned write at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

int Simulator::storeConditionalD(uint64_t addr, int64_t value,
                                 SimInstruction* instr) {
  // Correct behavior in this case, as defined by architecture, is to just
  // return 0, but there is no point at allowing that. It is certainly an
  // indicator of a bug.
  if (addr != LLAddr_) {
    printf("SC to bad address: 0x%016" PRIx64 ", pc=0x%016" PRIx64
           ", expected: 0x%016" PRIx64 "\n",
           addr, reinterpret_cast<intptr_t>(instr), LLAddr_);
    MOZ_CRASH();
  }

  if ((addr & kPointerAlignmentMask) == 0) {
    SharedMem<int64_t*> ptr =
        SharedMem<int64_t*>::shared(reinterpret_cast<int64_t*>(addr));

    if (!LLBit_) {
      return 1;
    }

    LLBit_ = false;
    LLAddr_ = 0;
    int64_t expected = lastLLValue_;
    int64_t old =
        AtomicOperations::compareExchangeSeqCst(ptr, expected, int64_t(value));
    return (old == expected) ? 0 : 1;
  }
  printf("Unaligned SC at 0x%016" PRIx64 ", pc=0x%016" PRIxPTR "\n", addr,
         reinterpret_cast<intptr_t>(instr));
  MOZ_CRASH();
  return 0;
}

void Simulator::DecodeRVRAType() {
  // TODO(riscv): Add macro for RISCV A extension
  // Special handling for A extension instructions because it uses func5
  // For all A extension instruction, V8 simulator is pure sequential. No
  // Memory address lock or other synchronizaiton behaviors.
  switch (instr_.InstructionBits() & kRATypeMask) {
    case RO_LR_W: {
      sreg_t addr = rs1();
      set_rd(loadLinkedW(addr, &instr_));
      TraceLr(addr, getRegister(rd_reg()), getRegister(rd_reg()));
      break;
    }
    case RO_SC_W: {
      sreg_t addr = rs1();
      auto value = static_cast<int32_t>(rs2());
      auto result =
          storeConditionalW(addr, static_cast<int32_t>(rs2()), &instr_);
      set_rd(result);
      if (!result) {
        TraceSc(addr, value);
      }
      break;
    }
    case RO_AMOSWAP_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return (uint32_t)rs2(); }, instr_.instr(),
          WORD)));
      break;
    }
    case RO_AMOADD_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs + (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOXOR_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs ^ (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOAND_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs & (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOOR_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return lhs | (uint32_t)rs2(); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMIN_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<int32_t>(
          rs1(), [&](int32_t lhs) { return std::min(lhs, (int32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMAX_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<int32_t>(
          rs1(), [&](int32_t lhs) { return std::max(lhs, (int32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMINU_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return std::min(lhs, (uint32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
    case RO_AMOMAXU_W: {
      if ((rs1() & 0x3) != 0) {
        DieOrDebug();
      }
      set_rd(sext32(amo<uint32_t>(
          rs1(), [&](uint32_t lhs) { return std::max(lhs, (uint32_t)rs2()); },
          instr_.instr(), WORD)));
      break;
    }
#  ifdef JS_CODEGEN_RISCV64
    case RO_LR_D: {
      sreg_t addr = rs1();
      set_rd(loadLinkedD(addr, &instr_));
      TraceLr(addr, getRegister(rd_reg()), getRegister(rd_reg()));
      break;
    }
    case RO_SC_D: {
      sreg_t addr = rs1();
      auto value = static_cast<int64_t>(rs2());
      auto result =
          storeConditionalD(addr, static_cast<int64_t>(rs2()), &instr_);
      set_rd(result);
      if (!result) {
        TraceSc(addr, value);
      }
      break;
    }
    case RO_AMOSWAP_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return rs2(); }, instr_.instr(), DWORD));
      break;
    }
    case RO_AMOADD_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs + rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOXOR_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs ^ rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOAND_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs & rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOOR_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return lhs | rs2(); }, instr_.instr(),
          DWORD));
      break;
    }
    case RO_AMOMIN_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return std::min(lhs, rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMAX_D: {
      set_rd(amo<int64_t>(
          rs1(), [&](int64_t lhs) { return std::max(lhs, rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMINU_D: {
      set_rd(amo<uint64_t>(
          rs1(), [&](uint64_t lhs) { return std::min(lhs, (uint64_t)rs2()); },
          instr_.instr(), DWORD));
      break;
    }
    case RO_AMOMAXU_D: {
      set_rd(amo<uint64_t>(
          rs1(), [&](uint64_t lhs) { return std::max(lhs, (uint64_t)rs2()); },
          instr_.instr(), DWORD));
      break;
    }
#  endif /*JS_CODEGEN_RISCV64*/
    // TODO(riscv): End Add macro for RISCV A extension
    default: {
      UNSUPPORTED();
    }
  }
}

void Simulator::DecodeRVRFPType() {
  // OP_FP instructions (F/D) uses func7 first. Some further uses func3 and
  // rs2()

  // kRATypeMask is only for func7
  switch (instr_.InstructionBits() & kRFPTypeMask) {
    // TODO(riscv): Add macro for RISCV F extension
    case RO_FADD_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fadd(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return frs1 + frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FSUB_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fsub(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return frs1 - frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FMUL_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fmul(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return frs1 * frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FDIV_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2) {
        if (is_invalid_fdiv(frs1, frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else if (frs2 == 0.0f) {
          this->set_fflags(kDivideByZero);
          return (std::signbit(frs1) == std::signbit(frs2)
                      ? std::numeric_limits<float>::infinity()
                      : -std::numeric_limits<float>::infinity());
        } else {
          return frs1 / frs2;
        }
      };
      set_frd(CanonicalizeFPUOp2<float>(fn));
      break;
    }
    case RO_FSQRT_S: {
      if (instr_.Rs2Value() == 0b00000) {
        // TODO(riscv): use rm value (round mode)
        auto fn = [this](float frs) {
          if (is_invalid_fsqrt(frs)) {
            this->set_fflags(kInvalidOperation);
            return std::numeric_limits<float>::quiet_NaN();
          } else {
            return std::sqrt(frs);
          }
        };
        set_frd(CanonicalizeFPUOp1<float>(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FSGNJ_S: {  // RO_FSGNJN_S  RO_FSQNJX_S
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FSGNJ_S
          set_frd(fsgnj32(frs1_boxed(), frs2_boxed(), false, false));
          break;
        }
        case 0b001: {  // RO_FSGNJN_S
          set_frd(fsgnj32(frs1_boxed(), frs2_boxed(), true, false));
          break;
        }
        case 0b010: {  // RO_FSQNJX_S
          set_frd(fsgnj32(frs1_boxed(), frs2_boxed(), false, true));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMIN_S: {  // RO_FMAX_S
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FMIN_S
          set_frd(FMaxMinHelper(frs1(), frs2(), MaxMinKind::kMin));
          break;
        }
        case 0b001: {  // RO_FMAX_S
          set_frd(FMaxMinHelper(frs1(), frs2(), MaxMinKind::kMax));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_W_S: {  // RO_FCVT_WU_S , 64F RO_FCVT_L_S RO_FCVT_LU_S
      float original_val = frs1();
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_W_S
          set_rd(RoundF2IHelper<int32_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00001: {  // RO_FCVT_WU_S
          set_rd(sext32(
              RoundF2IHelper<uint32_t>(original_val, instr_.RoundMode())));
          break;
        }
#  ifdef JS_CODEGEN_RISCV64
        case 0b00010: {  // RO_FCVT_L_S
          set_rd(RoundF2IHelper<int64_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00011: {  // RO_FCVT_LU_S
          set_rd(RoundF2IHelper<uint64_t>(original_val, instr_.RoundMode()));
          break;
        }
#  endif /* JS_CODEGEN_RISCV64 */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMV: {  // RO_FCLASS_S
      switch (instr_.Funct3Value()) {
        case 0b000: {
          if (instr_.Rs2Value() == 0b00000) {
            // RO_FMV_X_W
            set_rd(sext32(getFpuRegister(rs1_reg())));
          } else {
            UNSUPPORTED();
          }
          break;
        }
        case 0b001: {  // RO_FCLASS_S
          set_rd(FclassHelper(frs1()));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FLE_S: {  // RO_FEQ_S RO_FLT_S RO_FLE_S
      switch (instr_.Funct3Value()) {
        case 0b010: {  // RO_FEQ_S
          set_rd(CompareFHelper(frs1(), frs2(), EQ));
          break;
        }
        case 0b001: {  // RO_FLT_S
          set_rd(CompareFHelper(frs1(), frs2(), LT));
          break;
        }
        case 0b000: {  // RO_FLE_S
          set_rd(CompareFHelper(frs1(), frs2(), LE));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_S_W: {  // RO_FCVT_S_WU , 64F RO_FCVT_S_L RO_FCVT_S_LU
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_S_W
          set_frd(static_cast<float>((int32_t)rs1()));
          break;
        }
        case 0b00001: {  // RO_FCVT_S_WU
          set_frd(static_cast<float>((uint32_t)rs1()));
          break;
        }
#  ifdef JS_CODEGEN_RISCV64
        case 0b00010: {  // RO_FCVT_S_L
          set_frd(static_cast<float>((int64_t)rs1()));
          break;
        }
        case 0b00011: {  // RO_FCVT_S_LU
          set_frd(static_cast<float>((uint64_t)rs1()));
          break;
        }
#  endif /* JS_CODEGEN_RISCV64 */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMV_W_X: {
      if (instr_.Funct3Value() == 0b000) {
        // since FMV preserves source bit-pattern, no need to canonize
        Float32 result = Float32::FromBits((uint32_t)rs1());
        set_frd(result);
      } else {
        UNSUPPORTED();
      }
      break;
    }
      // TODO(riscv): Add macro for RISCV D extension
    case RO_FADD_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fadd(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return drs1 + drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FSUB_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fsub(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return drs1 - drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FMUL_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fmul(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return drs1 * drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FDIV_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2) {
        if (is_invalid_fdiv(drs1, drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else if (drs2 == 0.0) {
          this->set_fflags(kDivideByZero);
          return (std::signbit(drs1) == std::signbit(drs2)
                      ? std::numeric_limits<double>::infinity()
                      : -std::numeric_limits<double>::infinity());
        } else {
          return drs1 / drs2;
        }
      };
      set_drd(CanonicalizeFPUOp2<double>(fn));
      break;
    }
    case RO_FSQRT_D: {
      if (instr_.Rs2Value() == 0b00000) {
        // TODO(riscv): use rm value (round mode)
        auto fn = [this](double drs) {
          if (is_invalid_fsqrt(drs)) {
            this->set_fflags(kInvalidOperation);
            return std::numeric_limits<double>::quiet_NaN();
          } else {
            return std::sqrt(drs);
          }
        };
        set_drd(CanonicalizeFPUOp1<double>(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FSGNJ_D: {  // RO_FSGNJN_D RO_FSQNJX_D
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FSGNJ_D
          set_drd(fsgnj64(drs1_boxed(), drs2_boxed(), false, false));
          break;
        }
        case 0b001: {  // RO_FSGNJN_D
          set_drd(fsgnj64(drs1_boxed(), drs2_boxed(), true, false));
          break;
        }
        case 0b010: {  // RO_FSQNJX_D
          set_drd(fsgnj64(drs1_boxed(), drs2_boxed(), false, true));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FMIN_D: {  // RO_FMAX_D
      switch (instr_.Funct3Value()) {
        case 0b000: {  // RO_FMIN_D
          set_drd(FMaxMinHelper(drs1(), drs2(), MaxMinKind::kMin));
          break;
        }
        case 0b001: {  // RO_FMAX_D
          set_drd(FMaxMinHelper(drs1(), drs2(), MaxMinKind::kMax));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case (RO_FCVT_S_D & kRFPTypeMask): {
      if (instr_.Rs2Value() == 0b00001) {
        auto fn = [](double drs) { return static_cast<float>(drs); };
        set_frd(CanonicalizeDoubleToFloatOperation(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FCVT_D_S: {
      if (instr_.Rs2Value() == 0b00000) {
        auto fn = [](float frs) { return static_cast<double>(frs); };
        set_drd(CanonicalizeFloatToDoubleOperation(fn));
      } else {
        UNSUPPORTED();
      }
      break;
    }
    case RO_FLE_D: {  // RO_FEQ_D RO_FLT_D RO_FLE_D
      switch (instr_.Funct3Value()) {
        case 0b010: {  // RO_FEQ_S
          set_rd(CompareFHelper(drs1(), drs2(), EQ));
          break;
        }
        case 0b001: {  // RO_FLT_D
          set_rd(CompareFHelper(drs1(), drs2(), LT));
          break;
        }
        case 0b000: {  // RO_FLE_D
          set_rd(CompareFHelper(drs1(), drs2(), LE));
          break;
        }
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case (RO_FCLASS_D & kRFPTypeMask): {  // RO_FCLASS_D , 64D RO_FMV_X_D
      if (instr_.Rs2Value() != 0b00000) {
        UNSUPPORTED();
      }
      switch (instr_.Funct3Value()) {
        case 0b001: {  // RO_FCLASS_D
          set_rd(FclassHelper(drs1()));
          break;
        }
#  ifdef JS_CODEGEN_RISCV64
        case 0b000: {  // RO_FMV_X_D
          set_rd(bit_cast<int64_t>(drs1()));
          break;
        }
#  endif /* JS_CODEGEN_RISCV64 */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_W_D: {  // RO_FCVT_WU_D , 64F RO_FCVT_L_D RO_FCVT_LU_D
      double original_val = drs1();
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_W_D
          set_rd(RoundF2IHelper<int32_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00001: {  // RO_FCVT_WU_D
          set_rd(sext32(
              RoundF2IHelper<uint32_t>(original_val, instr_.RoundMode())));
          break;
        }
#  ifdef JS_CODEGEN_RISCV64
        case 0b00010: {  // RO_FCVT_L_D
          set_rd(RoundF2IHelper<int64_t>(original_val, instr_.RoundMode()));
          break;
        }
        case 0b00011: {  // RO_FCVT_LU_D
          set_rd(RoundF2IHelper<uint64_t>(original_val, instr_.RoundMode()));
          break;
        }
#  endif /* JS_CODEGEN_RISCV64 */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
    case RO_FCVT_D_W: {  // RO_FCVT_D_WU , 64F RO_FCVT_D_L RO_FCVT_D_LU
      switch (instr_.Rs2Value()) {
        case 0b00000: {  // RO_FCVT_D_W
          set_drd((int32_t)rs1());
          break;
        }
        case 0b00001: {  // RO_FCVT_D_WU
          set_drd((uint32_t)rs1());
          break;
        }
#  ifdef JS_CODEGEN_RISCV64
        case 0b00010: {  // RO_FCVT_D_L
          set_drd((int64_t)rs1());
          break;
        }
        case 0b00011: {  // RO_FCVT_D_LU
          set_drd((uint64_t)rs1());
          break;
        }
#  endif /* JS_CODEGEN_RISCV64 */
        default: {
          UNSUPPORTED();
        }
      }
      break;
    }
#  ifdef JS_CODEGEN_RISCV64
    case RO_FMV_D_X: {
      if (instr_.Funct3Value() == 0b000 && instr_.Rs2Value() == 0b00000) {
        // Since FMV preserves source bit-pattern, no need to canonize
        set_drd(bit_cast<double>(rs1()));
      } else {
        UNSUPPORTED();
      }
      break;
    }
#  endif /* JS_CODEGEN_RISCV64 */
    default: {
      UNSUPPORTED();
    }
  }
}

void Simulator::DecodeRVR4Type() {
  switch (instr_.InstructionBits() & kR4TypeMask) {
    // TODO(riscv): use F Extension macro block
    case RO_FMADD_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fadd(frs1 * frs2, frs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return std::fma(frs1, frs2, frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FMSUB_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fsub(frs1 * frs2, frs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return std::fma(frs1, frs2, -frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FNMSUB_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fsub(frs3, frs1 * frs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return -std::fma(frs1, frs2, -frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    case RO_FNMADD_S: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](float frs1, float frs2, float frs3) {
        if (is_invalid_fmul(frs1, frs2) || is_invalid_fadd(frs1 * frs2, frs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<float>::quiet_NaN();
        } else {
          return -std::fma(frs1, frs2, frs3);
        }
      };
      set_frd(CanonicalizeFPUOp3<float>(fn));
      break;
    }
    // TODO(riscv): use F Extension macro block
    case RO_FMADD_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fadd(drs1 * drs2, drs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return std::fma(drs1, drs2, drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FMSUB_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fsub(drs1 * drs2, drs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return std::fma(drs1, drs2, -drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FNMSUB_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fsub(drs3, drs1 * drs2)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return -std::fma(drs1, drs2, -drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    case RO_FNMADD_D: {
      // TODO(riscv): use rm value (round mode)
      auto fn = [this](double drs1, double drs2, double drs3) {
        if (is_invalid_fmul(drs1, drs2) || is_invalid_fadd(drs1 * drs2, drs3)) {
          this->set_fflags(kInvalidOperation);
          return std::numeric_limits<double>::quiet_NaN();
        } else {
          return -std::fma(drs1, drs2, drs3);
        }
      };
      set_drd(CanonicalizeFPUOp3<double>(fn));
      break;
    }
    default:
      UNSUPPORTED();
  }
}

#  ifdef CAN_USE_RVV_INSTRUCTIONS
bool Simulator::DecodeRvvVL() {
  uint32_t instr_temp =
      instr_.InstructionBits() & (kRvvMopMask | kRvvNfMask | kBaseOpcodeMask);
  if (RO_V_VL == instr_temp) {
    if (!(instr_.InstructionBits() & (kRvvRs2Mask))) {
      switch (instr_.vl_vs_width()) {
        case 8: {
          RVV_VI_LD(0, (i * nf + fn), int8, false);
          break;
        }
        case 16: {
          RVV_VI_LD(0, (i * nf + fn), int16, false);
          break;
        }
        case 32: {
          RVV_VI_LD(0, (i * nf + fn), int32, false);
          break;
        }
        case 64: {
          RVV_VI_LD(0, (i * nf + fn), int64, false);
          break;
        }
        default:
          UNIMPLEMENTED_RISCV();
          break;
      }
      return true;
    } else {
      UNIMPLEMENTED_RISCV();
      return true;
    }
  } else if (RO_V_VLS == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VLX == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VLSEG2 == instr_temp || RO_V_VLSEG3 == instr_temp ||
             RO_V_VLSEG4 == instr_temp || RO_V_VLSEG5 == instr_temp ||
             RO_V_VLSEG6 == instr_temp || RO_V_VLSEG7 == instr_temp ||
             RO_V_VLSEG8 == instr_temp) {
    if (!(instr_.InstructionBits() & (kRvvRs2Mask))) {
      UNIMPLEMENTED_RISCV();
      return true;
    } else {
      UNIMPLEMENTED_RISCV();
      return true;
    }
  } else if (RO_V_VLSSEG2 == instr_temp || RO_V_VLSSEG3 == instr_temp ||
             RO_V_VLSSEG4 == instr_temp || RO_V_VLSSEG5 == instr_temp ||
             RO_V_VLSSEG6 == instr_temp || RO_V_VLSSEG7 == instr_temp ||
             RO_V_VLSSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VLXSEG2 == instr_temp || RO_V_VLXSEG3 == instr_temp ||
             RO_V_VLXSEG4 == instr_temp || RO_V_VLXSEG5 == instr_temp ||
             RO_V_VLXSEG6 == instr_temp || RO_V_VLXSEG7 == instr_temp ||
             RO_V_VLXSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else {
    return false;
  }
}

bool Simulator::DecodeRvvVS() {
  uint32_t instr_temp =
      instr_.InstructionBits() & (kRvvMopMask | kRvvNfMask | kBaseOpcodeMask);
  if (RO_V_VS == instr_temp) {
    if (!(instr_.InstructionBits() & (kRvvRs2Mask))) {
      switch (instr_.vl_vs_width()) {
        case 8: {
          RVV_VI_ST(0, (i * nf + fn), uint8, false);
          break;
        }
        case 16: {
          RVV_VI_ST(0, (i * nf + fn), uint16, false);
          break;
        }
        case 32: {
          RVV_VI_ST(0, (i * nf + fn), uint32, false);
          break;
        }
        case 64: {
          RVV_VI_ST(0, (i * nf + fn), uint64, false);
          break;
        }
        default:
          UNIMPLEMENTED_RISCV();
          break;
      }
    } else {
      UNIMPLEMENTED_RISCV();
    }
    return true;
  } else if (RO_V_VSS == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSX == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSU == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSSEG2 == instr_temp || RO_V_VSSEG3 == instr_temp ||
             RO_V_VSSEG4 == instr_temp || RO_V_VSSEG5 == instr_temp ||
             RO_V_VSSEG6 == instr_temp || RO_V_VSSEG7 == instr_temp ||
             RO_V_VSSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSSSEG2 == instr_temp || RO_V_VSSSEG3 == instr_temp ||
             RO_V_VSSSEG4 == instr_temp || RO_V_VSSSEG5 == instr_temp ||
             RO_V_VSSSEG6 == instr_temp || RO_V_VSSSEG7 == instr_temp ||
             RO_V_VSSSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else if (RO_V_VSXSEG2 == instr_temp || RO_V_VSXSEG3 == instr_temp ||
             RO_V_VSXSEG4 == instr_temp || RO_V_VSXSEG5 == instr_temp ||
             RO_V_VSXSEG6 == instr_temp || RO_V_VSXSEG7 == instr_temp ||
             RO_V_VSXSEG8 == instr_temp) {
    UNIMPLEMENTED_RISCV();
    return true;
  } else {
    return false;
  }
}
#  endif

void Simulator::DecodeRVIType() {
  switch (instr_.InstructionBits() & kITypeMask) {
    case RO_JALR: {
      set_rd(get_pc() + kInstrSize);
      // Note: No need to shift 2 for JALR's imm12, but set lowest bit to 0.
      sreg_t next_pc = (rs1() + imm12()) & ~sreg_t(1);
      set_pc(next_pc);
      break;
    }
    case RO_LB: {
      sreg_t addr = rs1() + imm12();
      int8_t val = ReadMem<int8_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
    case RO_LH: {
      sreg_t addr = rs1() + imm12();
      int16_t val = ReadMem<int16_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
    case RO_LW: {
      sreg_t addr = rs1() + imm12();
      int32_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
    case RO_LBU: {
      sreg_t addr = rs1() + imm12();
      uint8_t val = ReadMem<uint8_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
    case RO_LHU: {
      sreg_t addr = rs1() + imm12();
      uint16_t val = ReadMem<uint16_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
#  ifdef JS_CODEGEN_RISCV64
    case RO_LWU: {
      int64_t addr = rs1() + imm12();
      uint32_t val = ReadMem<uint32_t>(addr, instr_.instr());
      set_rd(zext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
    case RO_LD: {
      int64_t addr = rs1() + imm12();
      int64_t val = ReadMem<int64_t>(addr, instr_.instr());
      set_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rd_reg()));
      break;
    }
#  endif /*JS_CODEGEN_RISCV64*/
    case RO_ADDI: {
      set_rd(sext_xlen(rs1() + imm12()));
      break;
    }
    case RO_SLTI: {
      set_rd(sreg_t(rs1()) < sreg_t(imm12()));
      break;
    }
    case RO_SLTIU: {
      set_rd(reg_t(rs1()) < reg_t(imm12()));
      break;
    }
    case RO_XORI: {
      set_rd(imm12() ^ rs1());
      break;
    }
    case RO_ORI: {
      set_rd(imm12() | rs1());
      break;
    }
    case RO_ANDI: {
      set_rd(imm12() & rs1());
      break;
    }
    case RO_SLLI: {
      require(shamt6() < xlen);
      set_rd(sext_xlen(rs1() << shamt6()));
      break;
    }
    case RO_SRLI: {  //  RO_SRAI
      if (!instr_.IsArithShift()) {
        require(shamt6() < xlen);
        set_rd(sext_xlen(zext_xlen(rs1()) >> shamt6()));
      } else {
        require(shamt6() < xlen);
        set_rd(sext_xlen(sext_xlen(rs1()) >> shamt6()));
      }
      break;
    }
#  ifdef JS_CODEGEN_RISCV64
    case RO_ADDIW: {
      set_rd(sext32(rs1() + imm12()));
      break;
    }
    case RO_SLLIW: {
      set_rd(sext32(rs1() << shamt5()));
      break;
    }
    case RO_SRLIW: {  //  RO_SRAIW
      if (!instr_.IsArithShift()) {
        set_rd(sext32(uint32_t(rs1()) >> shamt5()));
      } else {
        set_rd(sext32(int32_t(rs1()) >> shamt5()));
      }
      break;
    }
#  endif /*JS_CODEGEN_RISCV64*/
    case RO_FENCE: {
      // DO nothing in sumulator
      break;
    }
    case RO_ECALL: {                   // RO_EBREAK
      if (instr_.Imm12Value() == 0) {  // ECALL
        SoftwareInterrupt();
      } else if (instr_.Imm12Value() == 1) {  // EBREAK
        uint8_t code = get_ebreak_code(instr_.instr());
        if (code == kWasmTrapCode) {
          HandleWasmTrap();
        }
        SoftwareInterrupt();
      } else {
        UNSUPPORTED();
      }
      break;
    }
      // TODO(riscv): use Zifencei Standard Extension macro block
    case RO_FENCE_I: {
      // spike: flush icache.
      break;
    }
      // TODO(riscv): use Zicsr Standard Extension macro block
    case RO_CSRRW: {
      if (rd_reg() != zero_reg) {
        set_rd(zext_xlen(read_csr_value(csr_reg())));
      }
      write_csr_value(csr_reg(), rs1());
      break;
    }
    case RO_CSRRS: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (rs1_reg() != zero_reg) {
        set_csr_bits(csr_reg(), rs1());
      }
      break;
    }
    case RO_CSRRC: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (rs1_reg() != zero_reg) {
        clear_csr_bits(csr_reg(), rs1());
      }
      break;
    }
    case RO_CSRRWI: {
      if (rd_reg() != zero_reg) {
        set_rd(zext_xlen(read_csr_value(csr_reg())));
      }
      if (csr_reg() == csr_cycle) {
        if (imm5CSR() == kWasmTrapCode) {
          HandleWasmTrap();
          return;
        }
      }
      write_csr_value(csr_reg(), imm5CSR());
      break;
    }
    case RO_CSRRSI: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (imm5CSR() != 0) {
        set_csr_bits(csr_reg(), imm5CSR());
      }
      break;
    }
    case RO_CSRRCI: {
      set_rd(zext_xlen(read_csr_value(csr_reg())));
      if (imm5CSR() != 0) {
        clear_csr_bits(csr_reg(), imm5CSR());
      }
      break;
    }
    // TODO(riscv): use F Extension macro block
    case RO_FLW: {
      sreg_t addr = rs1() + imm12();
      uint32_t val = ReadMem<uint32_t>(addr, instr_.instr());
      set_frd(Float32::FromBits(val), false);
      TraceMemRdFloat(addr, Float32::FromBits(val), getFpuRegister(frd_reg()));
      break;
    }
    // TODO(riscv): use D Extension macro block
    case RO_FLD: {
      sreg_t addr = rs1() + imm12();
      uint64_t val = ReadMem<uint64_t>(addr, instr_.instr());
      set_drd(Float64::FromBits(val), false);
      TraceMemRdDouble(addr, Float64::FromBits(val), getFpuRegister(frd_reg()));
      break;
    }
    default: {
#  ifdef CAN_USE_RVV_INSTRUCTIONS
      if (!DecodeRvvVL()) {
        UNSUPPORTED();
      }
      break;
#  else
      UNSUPPORTED();
#  endif
    }
  }
}

void Simulator::DecodeRVSType() {
  switch (instr_.InstructionBits() & kSTypeMask) {
    case RO_SB:
      WriteMem<uint8_t>(rs1() + s_imm12(), (uint8_t)rs2(), instr_.instr());
      break;
    case RO_SH:
      WriteMem<uint16_t>(rs1() + s_imm12(), (uint16_t)rs2(), instr_.instr());
      break;
    case RO_SW:
      WriteMem<uint32_t>(rs1() + s_imm12(), (uint32_t)rs2(), instr_.instr());
      break;
#  ifdef JS_CODEGEN_RISCV64
    case RO_SD:
      WriteMem<uint64_t>(rs1() + s_imm12(), (uint64_t)rs2(), instr_.instr());
      break;
#  endif /*JS_CODEGEN_RISCV64*/
    // TODO(riscv): use F Extension macro block
    case RO_FSW: {
      WriteMem<Float32>(rs1() + s_imm12(), getFpuRegisterFloat32(rs2_reg()),
                        instr_.instr());
      break;
    }
    // TODO(riscv): use D Extension macro block
    case RO_FSD: {
      WriteMem<Float64>(rs1() + s_imm12(), getFpuRegisterFloat64(rs2_reg()),
                        instr_.instr());
      break;
    }
    default:
#  ifdef CAN_USE_RVV_INSTRUCTIONS
      if (!DecodeRvvVS()) {
        UNSUPPORTED();
      }
      break;
#  else
      UNSUPPORTED();
#  endif
  }
}

void Simulator::DecodeRVBType() {
  switch (instr_.InstructionBits() & kBTypeMask) {
    case RO_BEQ:
      if (rs1() == rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BNE:
      if (rs1() != rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BLT:
      if (rs1() < rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BGE:
      if (rs1() >= rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BLTU:
      if ((reg_t)rs1() < (reg_t)rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    case RO_BGEU:
      if ((reg_t)rs1() >= (reg_t)rs2()) {
        int64_t next_pc = get_pc() + boffset();
        set_pc(next_pc);
      }
      break;
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeRVUType() {
  // U Type doesn't have additoinal mask
  switch (instr_.BaseOpcodeFieldRaw()) {
    case LUI:
      set_rd(u_imm20());
      break;
    case AUIPC:
      set_rd(sext_xlen(u_imm20() + get_pc()));
      break;
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeRVJType() {
  // J Type doesn't have additional mask
  switch (instr_.BaseOpcodeValue()) {
    case JAL: {
      set_rd(get_pc() + kInstrSize);
      int64_t next_pc = get_pc() + imm20J();
      set_pc(next_pc);
      break;
    }
    default:
      UNSUPPORTED();
  }
}
void Simulator::DecodeCRType() {
  switch (instr_.RvcFunct4Value()) {
    case 0b1000:
      if (instr_.RvcRs1Value() != 0 && instr_.RvcRs2Value() == 0) {  // c.jr
        set_pc(rvc_rs1());
      } else if (instr_.RvcRdValue() != 0 &&
                 instr_.RvcRs2Value() != 0) {  // c.mv
        set_rvc_rd(sext_xlen(rvc_rs2()));
      } else {
        UNSUPPORTED();
      }
      break;
    case 0b1001:
      if (instr_.RvcRs1Value() == 0 && instr_.RvcRs2Value() == 0) {  // c.ebreak
        DieOrDebug();
      } else if (instr_.RvcRdValue() != 0 &&
                 instr_.RvcRs2Value() == 0) {  // c.jalr
        setRegister(ra, get_pc() + kShortInstrSize);
        set_pc(rvc_rs1());
      } else if (instr_.RvcRdValue() != 0 &&
                 instr_.RvcRs2Value() != 0) {  // c.add
        set_rvc_rd(sext_xlen(rvc_rs1() + rvc_rs2()));
      } else {
        UNSUPPORTED();
      }
      break;
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCAType() {
  switch (instr_.InstructionBits() & kCATypeMask) {
    case RO_C_SUB:
      set_rvc_rs1s(sext_xlen(rvc_rs1s() - rvc_rs2s()));
      break;
    case RO_C_XOR:
      set_rvc_rs1s(rvc_rs1s() ^ rvc_rs2s());
      break;
    case RO_C_OR:
      set_rvc_rs1s(rvc_rs1s() | rvc_rs2s());
      break;
    case RO_C_AND:
      set_rvc_rs1s(rvc_rs1s() & rvc_rs2s());
      break;
#  if JS_CODEGEN_RISCV64
    case RO_C_SUBW:
      set_rvc_rs1s(sext32(rvc_rs1s() - rvc_rs2s()));
      break;
    case RO_C_ADDW:
      set_rvc_rs1s(sext32(rvc_rs1s() + rvc_rs2s()));
      break;
#  endif
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCIType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_NOP_ADDI:
      if (instr_.RvcRdValue() == 0)  // c.nop
        break;
      else  // c.addi
        set_rvc_rd(sext_xlen(rvc_rs1() + rvc_imm6()));
      break;
#  if JS_CODEGEN_RISCV64
    case RO_C_ADDIW:
      set_rvc_rd(sext32(rvc_rs1() + rvc_imm6()));
      break;
#  endif
    case RO_C_LI:
      set_rvc_rd(sext_xlen(rvc_imm6()));
      break;
    case RO_C_LUI_ADD:
      if (instr_.RvcRdValue() == 2) {
        // c.addi16sp
        int64_t value = getRegister(sp) + rvc_imm6_addi16sp();
        setRegister(sp, value);
      } else if (instr_.RvcRdValue() != 0 && instr_.RvcRdValue() != 2) {
        // c.lui
        set_rvc_rd(rvc_u_imm6());
      } else {
        UNSUPPORTED();
      }
      break;
    case RO_C_SLLI:
      set_rvc_rd(sext_xlen(rvc_rs1() << rvc_shamt6()));
      break;
    case RO_C_FLDSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_ldsp();
      uint64_t val = ReadMem<uint64_t>(addr, instr_.instr());
      set_rvc_drd(Float64::FromBits(val), false);
      TraceMemRdDouble(addr, Float64::FromBits(val),
                       getFpuRegister(rvc_frd_reg()));
      break;
    }
#  if JS_CODEGEN_RISCV64
    case RO_C_LWSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_lwsp();
      int64_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rvc_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rvc_rd_reg()));
      break;
    }
    case RO_C_LDSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_ldsp();
      int64_t val = ReadMem<int64_t>(addr, instr_.instr());
      set_rvc_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rvc_rd_reg()));
      break;
    }
#  elif JS_CODEGEN_RISCV32
    case RO_C_FLWSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_ldsp();
      uint32_t val = ReadMem<uint32_t>(addr, instr_.instr());
      set_rvc_frd(Float32::FromBits(val), false);
      TraceMemRdFloat(addr, Float32::FromBits(val),
                      getFpuRegister(rvc_frd_reg()));
      break;
    }
    case RO_C_LWSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_lwsp();
      int32_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rvc_rd(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rvc_rd_reg()));
      break;
    }
#  endif
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCIWType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_ADDI4SPN: {
      set_rvc_rs2s(getRegister(sp) + rvc_imm8_addi4spn());
      break;
      default:
        UNSUPPORTED();
    }
  }
}

void Simulator::DecodeCSSType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_FSDSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_sdsp();
      WriteMem<Float64>(addr, getFpuRegisterFloat64(rvc_rs2_reg()),
                        instr_.instr());
      break;
    }
#  if JS_CODEGEN_RISCV32
    case RO_C_FSWSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_sdsp();
      WriteMem<Float32>(addr, getFpuRegisterFloat32(rvc_rs2_reg()),
                        instr_.instr());
      break;
    }
#  endif
    case RO_C_SWSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_swsp();
      WriteMem<int32_t>(addr, (int32_t)rvc_rs2(), instr_.instr());
      break;
    }
#  if JS_CODEGEN_RISCV64
    case RO_C_SDSP: {
      sreg_t addr = getRegister(sp) + rvc_imm6_sdsp();
      WriteMem<int64_t>(addr, (int64_t)rvc_rs2(), instr_.instr());
      break;
    }
#  endif
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCLType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_LW: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_w();
      int64_t val = ReadMem<int32_t>(addr, instr_.instr());
      set_rvc_rs2s(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rvc_rs2s_reg()));
      break;
    }
    case RO_C_FLD: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_d();
      uint64_t val = ReadMem<uint64_t>(addr, instr_.instr());
      set_rvc_drs2s(Float64::FromBits(val), false);
      break;
    }
#  if JS_CODEGEN_RISCV64
    case RO_C_LD: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_d();
      int64_t val = ReadMem<int64_t>(addr, instr_.instr());
      set_rvc_rs2s(sext_xlen(val), false);
      TraceMemRd(addr, val, getRegister(rvc_rs2s_reg()));
      break;
    }
#  elif JS_CODEGEN_RISCV32
    case RO_C_FLW: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_d();
      uint32_t val = ReadMem<uint32_t>(addr, instr_.instr());
      set_rvc_frs2s(Float32::FromBits(val), false);
      break;
    }
#  endif
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCSType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_SW: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_w();
      WriteMem<int32_t>(addr, (int32_t)rvc_rs2s(), instr_.instr());
      break;
    }
#  if JS_CODEGEN_RISCV64
    case RO_C_SD: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_d();
      WriteMem<int64_t>(addr, (int64_t)rvc_rs2s(), instr_.instr());
      break;
    }
#  endif
    case RO_C_FSD: {
      sreg_t addr = rvc_rs1s() + rvc_imm5_d();
      WriteMem<double>(addr, static_cast<double>(rvc_drs2s()), instr_.instr());
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCJType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_J: {
      set_pc(get_pc() + instr_.RvcImm11CJValue());
      break;
    }
    default:
      UNSUPPORTED();
  }
}

void Simulator::DecodeCBType() {
  switch (instr_.RvcOpcode()) {
    case RO_C_BNEZ:
      if (rvc_rs1() != 0) {
        sreg_t next_pc = get_pc() + rvc_imm8_b();
        set_pc(next_pc);
      }
      break;
    case RO_C_BEQZ:
      if (rvc_rs1() == 0) {
        sreg_t next_pc = get_pc() + rvc_imm8_b();
        set_pc(next_pc);
      }
      break;
    case RO_C_MISC_ALU:
      if (instr_.RvcFunct2BValue() == 0b00) {  // c.srli
        set_rvc_rs1s(sext_xlen(sext_xlen(rvc_rs1s()) >> rvc_shamt6()));
      } else if (instr_.RvcFunct2BValue() == 0b01) {  // c.srai
        require(rvc_shamt6() < xlen);
        set_rvc_rs1s(sext_xlen(sext_xlen(rvc_rs1s()) >> rvc_shamt6()));
      } else if (instr_.RvcFunct2BValue() == 0b10) {  // c.andi
        set_rvc_rs1s(rvc_imm6() & rvc_rs1s());
      } else {
        UNSUPPORTED();
      }
      break;
    default:
      UNSUPPORTED();
  }
}

void Simulator::callInternal(uint8_t* entry) {
  // Prepare to execute the code at entry.
  setRegister(pc, reinterpret_cast<int64_t>(entry));
  // Put down marker for end of simulation. The simulator will stop simulation
  // when the PC reaches this value. By saving the "end simulation" value into
  // the LR the simulation stops when returning to this call point.
  setRegister(ra, end_sim_pc);
  // Remember the values of callee-saved registers.
  intptr_t s0_val = getRegister(Simulator::Register::fp);
  intptr_t s1_val = getRegister(Simulator::Register::s1);
  intptr_t s2_val = getRegister(Simulator::Register::s2);
  intptr_t s3_val = getRegister(Simulator::Register::s3);
  intptr_t s4_val = getRegister(Simulator::Register::s4);
  intptr_t s5_val = getRegister(Simulator::Register::s5);
  intptr_t s6_val = getRegister(Simulator::Register::s6);
  intptr_t s7_val = getRegister(Simulator::Register::s7);
  intptr_t s8_val = getRegister(Simulator::Register::s8);
  intptr_t s9_val = getRegister(Simulator::Register::s9);
  intptr_t s10_val = getRegister(Simulator::Register::s10);
  intptr_t s11_val = getRegister(Simulator::Register::s11);
  intptr_t gp_val = getRegister(Simulator::Register::gp);
  intptr_t sp_val = getRegister(Simulator::Register::sp);

  // Set up the callee-saved registers with a known value. To be able to check
  // that they are preserved properly across JS execution. If this value is
  // small int, it should be SMI.
  intptr_t callee_saved_value = icount_;
  setRegister(Simulator::Register::fp, callee_saved_value);
  setRegister(Simulator::Register::s1, callee_saved_value);
  setRegister(Simulator::Register::s2, callee_saved_value);
  setRegister(Simulator::Register::s3, callee_saved_value);
  setRegister(Simulator::Register::s4, callee_saved_value);
  setRegister(Simulator::Register::s5, callee_saved_value);
  setRegister(Simulator::Register::s6, callee_saved_value);
  setRegister(Simulator::Register::s7, callee_saved_value);
  setRegister(Simulator::Register::s8, callee_saved_value);
  setRegister(Simulator::Register::s9, callee_saved_value);
  setRegister(Simulator::Register::s10, callee_saved_value);
  setRegister(Simulator::Register::s11, callee_saved_value);
  setRegister(Simulator::Register::gp, callee_saved_value);

  // Start the simulation.
  if (Simulator::StopSimAt != -1) {
    execute<true>();
  } else {
    execute<false>();
  }

  // Check that the callee-saved registers have been preserved.
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::fp));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s1));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s2));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s3));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s4));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s5));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s6));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s7));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s8));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s9));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s10));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::s11));
  MOZ_ASSERT(callee_saved_value == getRegister(Simulator::Register::gp));

  // Restore callee-saved registers with the original value.
  setRegister(Simulator::Register::fp, s0_val);
  setRegister(Simulator::Register::s1, s1_val);
  setRegister(Simulator::Register::s2, s2_val);
  setRegister(Simulator::Register::s3, s3_val);
  setRegister(Simulator::Register::s4, s4_val);
  setRegister(Simulator::Register::s5, s5_val);
  setRegister(Simulator::Register::s6, s6_val);
  setRegister(Simulator::Register::s7, s7_val);
  setRegister(Simulator::Register::s8, s8_val);
  setRegister(Simulator::Register::s9, s9_val);
  setRegister(Simulator::Register::s10, s10_val);
  setRegister(Simulator::Register::s11, s11_val);
  setRegister(Simulator::Register::gp, gp_val);
  setRegister(Simulator::Register::sp, sp_val);
}

int64_t Simulator::call(uint8_t* entry, int argument_count, ...) {
  va_list parameters;
  va_start(parameters, argument_count);

  int64_t original_stack = getRegister(sp);
  // Compute position of stack on entry to generated code.
  int64_t entry_stack = original_stack;
  if (argument_count > kCArgSlotCount) {
    entry_stack = entry_stack - argument_count * sizeof(int64_t);
  } else {
    entry_stack = entry_stack - kCArgsSlotsSize;
  }

  entry_stack &= ~U64(ABIStackAlignment - 1);

  intptr_t* stack_argument = reinterpret_cast<intptr_t*>(entry_stack);

  // Setup the arguments.
  for (int i = 0; i < argument_count; i++) {
    js::jit::Register argReg;
    if (GetIntArgReg(i, &argReg)) {
      setRegister(argReg.code(), va_arg(parameters, int64_t));
    } else {
      stack_argument[i] = va_arg(parameters, int64_t);
    }
  }

  va_end(parameters);
  setRegister(sp, entry_stack);

  callInternal(entry);

  // Pop stack passed arguments.
  MOZ_ASSERT(entry_stack == getRegister(sp));
  setRegister(sp, original_stack);

  int64_t result = getRegister(a0);
  return result;
}

uintptr_t Simulator::pushAddress(uintptr_t address) {
  int new_sp = getRegister(sp) - sizeof(uintptr_t);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(new_sp);
  *stack_slot = address;
  setRegister(sp, new_sp);
  return new_sp;
}

uintptr_t Simulator::popAddress() {
  int current_sp = getRegister(sp);
  uintptr_t* stack_slot = reinterpret_cast<uintptr_t*>(current_sp);
  uintptr_t address = *stack_slot;
  setRegister(sp, current_sp + sizeof(uintptr_t));
  return address;
}

}  // namespace jit
}  // namespace js

js::jit::Simulator* JSContext::simulator() const { return simulator_; }

#endif  // JS_SIMULATOR_RISCV64
