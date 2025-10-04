/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef wasm_WasmSummarizeInsn_h
#define wasm_WasmSummarizeInsn_h

#include "mozilla/Maybe.h"
#include "wasm/WasmCodegenTypes.h"  // TrapMachineInsn

namespace js {
namespace wasm {

#ifdef DEBUG

// Inspect the machine instruction at `insn` and return a classification as to
// what it is.  If the instruction can't be identified, return
// `mozilla::Nothing`.  If the instruction is identified, the identification
// must be correct -- in other words, if a `mozilla::Some(i)` is returned, `i`
// must be the correct classification for the instruction.  Return
// `mozilla::Nothing` in case of doubt.
//
// This function is only used by ModuleGenerator::finishCodeTier to audit wasm
// trap sites.  So it doesn't need to handle the whole complexity of the
// machine's instruction set.  It only needs to handle the tiny sub-dialect
// used by the trappable instructions we actually generate.
mozilla::Maybe<TrapMachineInsn> SummarizeTrapInstruction(const uint8_t* insn);

#endif

}  // namespace wasm
}  // namespace js

#endif /* wasm_WasmSummarizeInsn_h */
