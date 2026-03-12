/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2016 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// This is an INTERNAL header for Wasm baseline compiler: common configuration
// and simple definitions; all include directives.

#ifndef wasm_wasm_baseline_defs_h
#define wasm_wasm_baseline_defs_h

#include <algorithm>
#include <utility>

#include "jit/AtomicOp.h"
#include "jit/IonTypes.h"
#include "jit/JitAllocPolicy.h"
#include "jit/Label.h"
#include "jit/RegisterAllocator.h"
#include "jit/Registers.h"
#include "jit/RegisterSets.h"
#if defined(JS_CODEGEN_ARM)
#  include "jit/arm/Assembler-arm.h"
#endif
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
#  include "jit/x86-shared/Architecture-x86-shared.h"
#  include "jit/x86-shared/Assembler-x86-shared.h"
#endif
#if defined(JS_CODEGEN_MIPS64)
#  include "jit/mips-shared/Assembler-mips-shared.h"
#  include "jit/mips64/Assembler-mips64.h"
#endif
#if defined(JS_CODEGEN_LOONG64)
#  include "jit/loong64/Assembler-loong64.h"
#endif
#if defined(JS_CODEGEN_RISCV64)
#  include "jit/riscv64/Assembler-riscv64.h"
#endif
#include "js/ScalarType.h"
#include "util/Memory.h"
#include "wasm/WasmCodegenTypes.h"
#include "wasm/WasmDebugFrame.h"
#include "wasm/WasmGC.h"
#include "wasm/WasmGcObject.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmInstance.h"
#include "wasm/WasmOpIter.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmStubs.h"
#include "wasm/WasmValidate.h"

namespace js {
namespace wasm {

using HandleNaNSpecially = bool;
using InvertBranch = bool;
using IsKnownNotZero = bool;
using IsUnsigned = bool;
using IsRemainder = bool;
using NeedsBoundsCheck = bool;
using WantResult = bool;
using ZeroOnOverflow = bool;

class BaseStackFrame;

// Two flags, useABI and restoreRegisterStateAndRealm, control how calls are
// made.
//
// UseABI::Wasm implies that the Instance/Heap/Global registers are nonvolatile,
// except when RestoreRegisterStateAndRealm::True is also set, when they are
// volatile.
//
// UseABI::Builtin implies that the Instance/Heap/Global registers are volatile.
// In this case, we require RestoreRegisterStateAndRealm::False.  The calling
// convention is otherwise like UseABI::Wasm.
//
// UseABI::System implies that the Instance/Heap/Global registers are volatile.
// Additionally, the parameter passing mechanism may be slightly different from
// the UseABI::Wasm convention.
//
// When the Instance/Heap/Global registers are not volatile, the baseline
// compiler will restore the Instance register from its save slot before the
// call, since the baseline compiler uses the Instance register for other
// things.
//
// When those registers are volatile, the baseline compiler will reload them
// after the call (it will restore the Instance register from the save slot and
// load the other two from the Instance data).

enum class UseABI { Wasm, Builtin, System };
enum class RestoreRegisterStateAndRealm { False = false, True = true };
enum class RhsDestOp { True = true };

// Compiler configuration.
//
// The following internal configuration #defines are used.  The configuration is
// partly below in this file, partly in WasmBCRegDefs.h.
//
// RABALDR_PIN_INSTANCE
//   InstanceReg is not allocatable and always holds the current Instance*,
//   except in known contexts where it could have been clobbered, such as after
//   certain calls.
//
// RABALDR_ZERO_EXTENDS
//   The canonical representation of a 32-bit value in a 64-bit register is
//   zero-extended.  For 64-bit platforms only.  See comment block "64-bit GPRs
//   carrying 32-bit values" in MacroAssembler.h.
//
// RABALDR_CHUNKY_STACK
//   The platform must allocate the CPU stack in chunks and not word-at-a-time
//   due to SP alignment requirements (ARM64 for now).
//
// RABALDR_INT_DIV_I64_CALLOUT
//   The platform calls out to the runtime to divide i64/u64.
//
// RABALDR_I64_TO_FLOAT_CALLOUT
//   The platform calls out to the runtime for i64 -> fXX conversions.
//
// RABALDR_FLOAT_TO_I64_CALLOUT
//   The platform calls out to the runtime for fXX -> i64 conversions.
//
// RABALDR_SCRATCH_<TypeName>
//   The baseline compiler has its own scratch registers for the given type, it
//   does not use the MacroAssembler's scratch.  This is really an anachronism -
//   the baseline compiler should never use the MacroAssembler's scratches.
//
// RABALDR_SCRATCH_F32_ALIASES_F64
//   On a platform where the baseline compiler has its own F32 and F64
//   scratches, these are the same register.

#ifdef JS_CODEGEN_X64
#  define RABALDR_ZERO_EXTENDS
#  define RABALDR_PIN_INSTANCE
#endif

#ifdef JS_CODEGEN_ARM64
#  define RABALDR_CHUNKY_STACK
#  define RABALDR_ZERO_EXTENDS
#  define RABALDR_PIN_INSTANCE
#endif

#ifdef JS_CODEGEN_X86
#  define RABALDR_INT_DIV_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_ARM
#  define RABALDR_INT_DIV_I64_CALLOUT
#  define RABALDR_I64_TO_FLOAT_CALLOUT
#  define RABALDR_FLOAT_TO_I64_CALLOUT
#endif

#ifdef JS_CODEGEN_MIPS64
#  define RABALDR_PIN_INSTANCE
#endif

#ifdef JS_CODEGEN_LOONG64
#  define RABALDR_PIN_INSTANCE
#endif

#ifdef JS_CODEGEN_RISCV64
#  define RABALDR_PIN_INSTANCE
#endif

// Max number of pushes onto the value stack for any opcode or emitter that
// does not push a variable, unbounded amount (anything with multiple
// results).  This includes also intermediate pushes such as values pushed as
// parameters for builtin calls.
//
// This limit is set quite high on purpose, so as to avoid brittleness.  The
// true max value is likely no more than four or five.

static constexpr size_t MaxPushesPerOpcode = 10;

}  // namespace wasm
}  // namespace js

#endif  // wasm_wasm_baseline_defs_h
