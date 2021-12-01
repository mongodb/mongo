// Copyright 2014, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifdef JS_SIMULATOR_ARM64

#ifndef VIXL_A64_DEBUGGER_A64_H_
#define VIXL_A64_DEBUGGER_A64_H_

#include <ctype.h>
#include <errno.h>
#include <limits.h>

#include "jit/arm64/vixl/Constants-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Simulator-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

namespace vixl {

// Flags that represent the debugger state.
enum DebugParameters {
  DBG_INACTIVE = 0,
  DBG_ACTIVE = 1 << 0,  // The debugger is active.
  DBG_BREAK  = 1 << 1   // The debugger is at a breakpoint.
};

// Forward declarations.
class DebugCommand;
class Token;
class FormatToken;

class Debugger : public Simulator {
 public:
  explicit Debugger(JSContext* cx, Decoder* decoder, FILE* stream = stdout);
  ~Debugger();

  virtual void Run() override;
  virtual void VisitException(const Instruction* instr) override;

  int debug_parameters() const { return debug_parameters_; }
  void set_debug_parameters(int parameters) {
    debug_parameters_ = parameters;

    update_pending_request();
  }

  // Numbers of instructions to execute before the debugger shell is given
  // back control.
  int64_t steps() const { return steps_; }
  void set_steps(int64_t value) {
    VIXL_ASSERT(value > 1);
    steps_ = value;
  }

  bool IsDebuggerRunning() const {
    return (debug_parameters_ & DBG_ACTIVE) != 0;
  }

  bool pending_request() const { return pending_request_; }
  void update_pending_request() {
    pending_request_ = IsDebuggerRunning();
  }

  void PrintInstructions(const void* address, int64_t count = 1);
  void PrintMemory(const uint8_t* address,
                   const FormatToken* format,
                   int64_t count = 1);
  void PrintRegister(const Register& target_reg,
                     const char* name,
                     const FormatToken* format);
  void PrintFPRegister(const FPRegister& target_fpreg,
                       const FormatToken* format);

 private:
  char* ReadCommandLine(const char* prompt, char* buffer, int length);
  void RunDebuggerShell();
  void DoBreakpoint(const Instruction* instr);

  int debug_parameters_;
  bool pending_request_;
  int64_t steps_;
  DebugCommand* last_command_;
  PrintDisassembler* disasm_;
  Decoder* printer_;

  // Length of the biggest command line accepted by the debugger shell.
  static const int kMaxDebugShellLine = 256;
};

}  // namespace vixl

#endif  // VIXL_A64_DEBUGGER_A64_H_

#endif  // JS_SIMULATOR_ARM64
