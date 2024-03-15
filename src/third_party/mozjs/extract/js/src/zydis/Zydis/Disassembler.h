/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Joel Hoener

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
 * All-in-one convenience function providing the simplest possible way to use Zydis.
 */

#ifndef ZYDIS_DISASSEMBLER_H
#define ZYDIS_DISASSEMBLER_H

#include "zydis/Zydis/Decoder.h"
#include "zydis/Zydis/Formatter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Types                                                                                          */
/* ============================================================================================== */

/**
 * All commonly used information about a decoded instruction that Zydis can provide.
 *
 * This structure is filled in by calling `ZydisDisassembleIntel` or `ZydisDisassembleATT`.
 */
typedef struct ZydisDisassembledInstruction_
{
    /**
     * The runtime address that was passed when disassembling the instruction.
     */
    ZyanU64 runtime_address;
    /**
     * General information about the decoded instruction in machine-readable format.
     */
    ZydisDecodedInstruction info;
    /**
     * The operands of the decoded instruction in a machine-readable format.
     *
     * The amount of actual operands can be determined by inspecting the corresponding fields
     * in the `info` member of this struct. Inspect `operand_count_visible` if you care about
     * visible operands (those that are printed by the formatter) or `operand_count` if you're
     * also interested in implicit operands (for example the registers implicitly accessed by
     * `pushad`). Unused entries are zeroed.
     */
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    /**
     * The textual, human-readable representation of the instruction.
     *
     * Guaranteed to be zero-terminated.
     */
    char text[96];
} ZydisDisassembledInstruction;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * Disassemble an instruction and format it to human-readable text in a single step (Intel syntax).
 *
 * @param machine_mode      The machine mode to assume when disassembling. When in doubt, pass
 *                          `ZYDIS_MACHINE_MODE_LONG_64` for what is typically referred to as
 *                          "64-bit mode" or `ZYDIS_MACHINE_MODE_LEGACY_32` for "32-bit mode".
 * @param runtime_address   The program counter (`eip` / `rip`) to assume when formatting the
 *                          instruction. Many instructions behave differently depending on the
 *                          address they are located at.
 * @param buffer            A pointer to the raw instruction bytes that you wish to decode.
 * @param length            The length of the input buffer. Note that this can be bigger than the
 *                          actual size of the instruction -- you don't have to know the size up
 *                          front. This length is merely used to prevent Zydis from doing
 *                          out-of-bounds reads on your buffer.
 * @param instruction       A pointer to receive the decoded instruction information. Can be
 *                          uninitialized and reused on later calls.
 *
 * This is a convenience function intended as a quick path for getting started with using Zydis.
 * It internally calls a range of other more advanced functions to obtain all commonly needed
 * information about the instruction. It is likely that you won't need most of this information in
 * practice, so it is advisable to instead call these more advanced functions directly if you're
 * concerned about performance.
 *
 * This function essentially combines the following more advanced functions into a single call:
 *
 *   - `ZydisDecoderInit`
 *   - `ZydisDecoderDecodeInstruction`
 *   - `ZydisDecoderDecodeOperands`
 *   - `ZydisFormatterInit`
 *   - `ZydisFormatterFormatInstruction`
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDisassembleIntel(ZydisMachineMode machine_mode,
    ZyanU64 runtime_address, const void* buffer, ZyanUSize length,
    ZydisDisassembledInstruction *instruction);

/**
 * Disassemble an instruction and format it to human-readable text in a single step (AT&T syntax).
 *
 * @copydetails ZydisDisassembleIntel
 */
ZYDIS_EXPORT ZyanStatus ZydisDisassembleATT(ZydisMachineMode machine_mode,
    ZyanU64 runtime_address, const void* buffer, ZyanUSize length,
    ZydisDisassembledInstruction *instruction);

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_DISASSEMBLER_H */
