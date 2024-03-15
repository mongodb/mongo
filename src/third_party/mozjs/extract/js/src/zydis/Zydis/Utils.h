/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd

 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

***************************************************************************************************/

/**
 * @file
 * Other utility functions.
 */

#ifndef ZYDIS_UTILS_H
#define ZYDIS_UTILS_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zydis/DecoderTypes.h"
#include "zydis/Zydis/Status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup utils Utils
 * Miscellaneous utility functions. Address translation and other helpers.
 * @{
 */

/* ---------------------------------------------------------------------------------------------- */
/* Address calculation                                                                            */
/* ---------------------------------------------------------------------------------------------- */

// TODO: Provide a function that works in minimal-mode and does not require a operand parameter

/**
 * Calculates the absolute address value for the given instruction operand.
 *
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand         A pointer to the `ZydisDecodedOperand` struct.
 * @param   runtime_address The runtime address of the instruction.
 * @param   result_address  A pointer to the memory that receives the absolute address.
 *
 * @return  A zyan status code.
 *
 * You should use this function in the following cases:
 * - `IMM` operands with relative address (e.g. `JMP`, `CALL`, ...)
 * - `MEM` operands with `RIP`/`EIP`-relative address (e.g. `MOV RAX, [RIP+0x12345678]`)
 * - `MEM` operands with absolute address (e.g. `MOV RAX, [0x12345678]`)
 *   - The displacement needs to get truncated and zero extended
 */
ZYDIS_EXPORT ZyanStatus ZydisCalcAbsoluteAddress(const ZydisDecodedInstruction* instruction,
    const ZydisDecodedOperand* operand, ZyanU64 runtime_address, ZyanU64* result_address);

/**
 * Calculates the absolute address value for the given instruction operand.
 *
 * @param   instruction         A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand             A pointer to the `ZydisDecodedOperand` struct.
 * @param   runtime_address     The runtime address of the instruction.
 * @param   register_context    A pointer to the `ZydisRegisterContext` struct.
 * @param   result_address      A pointer to the memory that receives the absolute target-address.
 *
 * @return  A zyan status code.
 *
 * This function behaves like `ZydisCalcAbsoluteAddress` but takes an additional register-context
 * argument to allow calculation of addresses depending on runtime register values.
 *
 * Note that `IP/EIP/RIP` from the register-context will be ignored in favor of the passed
 * runtime-address.
 */
ZYDIS_EXPORT ZyanStatus ZydisCalcAbsoluteAddressEx(const ZydisDecodedInstruction* instruction,
    const ZydisDecodedOperand* operand, ZyanU64 runtime_address,
    const ZydisRegisterContext* register_context, ZyanU64* result_address);

/* ---------------------------------------------------------------------------------------------- */

/**
 * @}
 */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_UTILS_H */
