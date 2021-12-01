// Copyright 2015, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice
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

#ifndef VIXL_A64_SIMULATOR_CONSTANTS_A64_H_
#define VIXL_A64_SIMULATOR_CONSTANTS_A64_H_

namespace vixl {

// Debug instructions.
//
// VIXL's macro-assembler and simulator support a few pseudo instructions to
// make debugging easier. These pseudo instructions do not exist on real
// hardware.
//
// TODO: Also consider allowing these pseudo-instructions to be disabled in the
// simulator, so that users can check that the input is a valid native code.
// (This isn't possible in all cases. Printf won't work, for example.)
//
// Each debug pseudo instruction is represented by a HLT instruction. The HLT
// immediate field is used to identify the type of debug pseudo instruction.

enum DebugHltOpcodes {
  kUnreachableOpcode = 0xdeb0,
  kPrintfOpcode,
  kTraceOpcode,
  kLogOpcode,
  // Aliases.
  kDebugHltFirstOpcode = kUnreachableOpcode,
  kDebugHltLastOpcode = kLogOpcode
};

// Each pseudo instruction uses a custom encoding for additional arguments, as
// described below.

// Unreachable - kUnreachableOpcode
//
// Instruction which should never be executed. This is used as a guard in parts
// of the code that should not be reachable, such as in data encoded inline in
// the instructions.

// Printf - kPrintfOpcode
//  - arg_count: The number of arguments.
//  - arg_pattern: A set of PrintfArgPattern values, packed into two-bit fields.
//
// Simulate a call to printf.
//
// Floating-point and integer arguments are passed in separate sets of registers
// in AAPCS64 (even for varargs functions), so it is not possible to determine
// the type of each argument without some information about the values that were
// passed in. This information could be retrieved from the printf format string,
// but the format string is not trivial to parse so we encode the relevant
// information with the HLT instruction.
//
// Also, the following registers are populated (as if for a native A64 call):
//    x0: The format string
// x1-x7: Optional arguments, if type == CPURegister::kRegister
// d0-d7: Optional arguments, if type == CPURegister::kFPRegister
const unsigned kPrintfArgCountOffset = 1 * kInstructionSize;
const unsigned kPrintfArgPatternListOffset = 2 * kInstructionSize;
const unsigned kPrintfLength = 3 * kInstructionSize;

const unsigned kPrintfMaxArgCount = 4;

// The argument pattern is a set of two-bit-fields, each with one of the
// following values:
enum PrintfArgPattern {
  kPrintfArgW = 1,
  kPrintfArgX = 2,
  // There is no kPrintfArgS because floats are always converted to doubles in C
  // varargs calls.
  kPrintfArgD = 3
};
static const unsigned kPrintfArgPatternBits = 2;

// Trace - kTraceOpcode
//  - parameter: TraceParameter stored as a uint32_t
//  - command: TraceCommand stored as a uint32_t
//
// Allow for trace management in the generated code. This enables or disables
// automatic tracing of the specified information for every simulated
// instruction.
const unsigned kTraceParamsOffset = 1 * kInstructionSize;
const unsigned kTraceCommandOffset = 2 * kInstructionSize;
const unsigned kTraceLength = 3 * kInstructionSize;

// Trace parameters.
enum TraceParameters {
  LOG_DISASM     = 1 << 0,  // Log disassembly.
  LOG_REGS       = 1 << 1,  // Log general purpose registers.
  LOG_VREGS      = 1 << 2,  // Log NEON and floating-point registers.
  LOG_SYSREGS    = 1 << 3,  // Log the flags and system registers.
  LOG_WRITE      = 1 << 4,  // Log writes to memory.

  LOG_NONE       = 0,
  LOG_STATE      = LOG_REGS | LOG_VREGS | LOG_SYSREGS,
  LOG_ALL        = LOG_DISASM | LOG_STATE | LOG_WRITE
};

// Trace commands.
enum TraceCommand {
  TRACE_ENABLE   = 1,
  TRACE_DISABLE  = 2
};

// Log - kLogOpcode
//  - parameter: TraceParameter stored as a uint32_t
//
// Print the specified information once. This mechanism is separate from Trace.
// In particular, _all_ of the specified registers are printed, rather than just
// the registers that the instruction writes.
//
// Any combination of the TraceParameters values can be used, except that
// LOG_DISASM is not supported for Log.
const unsigned kLogParamsOffset = 1 * kInstructionSize;
const unsigned kLogLength = 2 * kInstructionSize;
}  // namespace vixl

#endif  // VIXL_A64_SIMULATOR_CONSTANTS_A64_H_
