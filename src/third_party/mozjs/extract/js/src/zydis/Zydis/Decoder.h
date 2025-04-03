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
 * Functions for decoding instructions.
 */

#ifndef ZYDIS_DECODER_H
#define ZYDIS_DECODER_H

#include "zydis/Zycore/Types.h"
#include "zydis/Zycore/Defines.h"
#include "zydis/Zydis/DecoderTypes.h"
#include "zydis/Zydis/Status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Decoder mode                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisDecoderMode` enum.
 */
typedef enum ZydisDecoderMode_
{
    /**
     * Enables minimal instruction decoding without semantic analysis.
     *
     * This mode provides access to the mnemonic, the instruction-length, the effective
     * operand-size, the effective address-width, some attributes (e.g. `ZYDIS_ATTRIB_IS_RELATIVE`)
     * and all of the information in the `raw` field of the `ZydisDecodedInstruction` struct.
     *
     * Operands, most attributes and other specific information (like `AVX` info) are not
     * accessible in this mode.
     *
     * This mode is NOT enabled by default.
     */
    ZYDIS_DECODER_MODE_MINIMAL,
    /**
     * Enables the `AMD`-branch mode.
     *
     * Intel ignores the operand-size override-prefix (`0x66`) for all branches with 32-bit
     * immediates and forces the operand-size of the instruction to 64-bit in 64-bit mode.
     * In `AMD`-branch mode `0x66` is not ignored and changes the operand-size and the size of the
     * immediate to 16-bit.
     *
     * This mode is NOT enabled by default.
     */
    ZYDIS_DECODER_MODE_AMD_BRANCHES,
    /**
     * Enables `KNC` compatibility-mode.
     *
     * `KNC` and `KNL+` chips are sharing opcodes and encodings for some mask-related instructions.
     * Enable this mode to use the old `KNC` specifications (different mnemonics, operands, ..).
     *
     * This mode is NOT enabled by default.
     */
    ZYDIS_DECODER_MODE_KNC,
    /**
     * Enables the `MPX` mode.
     *
     * The `MPX` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_MPX,
    /**
     * Enables the `CET` mode.
     *
     * The `CET` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_CET,
    /**
     * Enables the `LZCNT` mode.
     *
     * The `LZCNT` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_LZCNT,
    /**
     * Enables the `TZCNT` mode.
     *
     * The `TZCNT` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_TZCNT,
    /**
     * Enables the `WBNOINVD` mode.
     *
     * The `WBINVD` instruction is interpreted as `WBNOINVD` on ICL chips, if a `F3` prefix is
     * used.
     *
     * This mode is disabled by default.
     */
    ZYDIS_DECODER_MODE_WBNOINVD,
     /**
     * Enables the `CLDEMOTE` mode.
     *
     * The `CLDEMOTE` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_CLDEMOTE,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_DECODER_MODE_MAX_VALUE = ZYDIS_DECODER_MODE_CLDEMOTE,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_DECODER_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_DECODER_MODE_MAX_VALUE)
} ZydisDecoderMode;

/* ---------------------------------------------------------------------------------------------- */
/* Decoder struct                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisDecoder` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZydisDecoder_
{
    /**
     * The machine mode.
     */
    ZydisMachineMode machine_mode;
    /**
     * The stack width.
     */
    ZydisStackWidth stack_width;
    /**
     * The decoder mode array.
     */
    ZyanBool decoder_mode[ZYDIS_DECODER_MODE_MAX_VALUE + 1];
} ZydisDecoder;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup decoder Decoder
 * Functions allowing decoding of instruction bytes to a machine interpretable struct.
 * @{
 */

/**
 * Initializes the given `ZydisDecoder` instance.
 *
 * @param   decoder         A pointer to the `ZydisDecoder` instance.
 * @param   machine_mode    The machine mode.
 * @param   stack_width     The stack width.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderInit(ZydisDecoder* decoder, ZydisMachineMode machine_mode,
    ZydisStackWidth stack_width);

/**
 * Enables or disables the specified decoder-mode.
 *
 * @param   decoder A pointer to the `ZydisDecoder` instance.
 * @param   mode    The decoder mode.
 * @param   enabled `ZYAN_TRUE` to enable, or `ZYAN_FALSE` to disable the specified decoder-mode.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderEnableMode(ZydisDecoder* decoder, ZydisDecoderMode mode,
    ZyanBool enabled);

/**
 * Decodes the instruction in the given input `buffer` and returns all details (e.g. operands).
 *
 * @param   decoder         A pointer to the `ZydisDecoder` instance.
 * @param   buffer          A pointer to the input buffer.
 * @param   length          The length of the input buffer. Note that this can be bigger than the
 *                          actual size of the instruction -- you don't have to know the size up
 *                          front. This length is merely used to prevent Zydis from doing
 *                          out-of-bounds reads on your buffer.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct receiving the details
 *                          about the decoded instruction.
 * @param   operands        A pointer to an array with `ZYDIS_MAX_OPERAND_COUNT` entries that
 *                          receives the decoded operands. The number of operands decoded is
 *                          determined by the `instruction.operand_count` field. Excess entries are
 *                          zeroed.
 *
 * This is a convenience function that combines the following functions into one call:
 * 
 *   - `ZydisDecoderDecodeInstruction`
 *   - `ZydisDecoderDecodeOperands`
 * 
 * Please refer to `ZydisDecoderDecodeInstruction` if operand decoding is not required or should
 * be done separately (`ZydisDecoderDecodeOperands`).
 *
 * This function is not available in MINIMAL_MODE.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderDecodeFull(const ZydisDecoder* decoder,
    const void* buffer, ZyanUSize length, ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]);

/**
 * Decodes the instruction in the given input `buffer`.
 *
 * @param   decoder     A pointer to the `ZydisDecoder` instance.
 * @param   context     A pointer to a decoder context struct which is required for further
 *                      decoding (e.g. operand decoding using `ZydisDecoderDecodeOperands`) or
 *                      `ZYAN_NULL` if not needed.
 * @param   buffer      A pointer to the input buffer.
 * @param   length      The length of the input buffer. Note that this can be bigger than the
 *                      actual size of the instruction -- you don't have to know the size up
 *                      front. This length is merely used to prevent Zydis from doing
 *                      out-of-bounds reads on your buffer.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct, that receives the
 *                      details about the decoded instruction.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderDecodeInstruction(const ZydisDecoder* decoder,
    ZydisDecoderContext* context, const void* buffer, ZyanUSize length,
    ZydisDecodedInstruction* instruction);

/**
 * Decodes the instruction operands.
 *
 * @param   decoder         A pointer to the `ZydisDecoder` instance.
 * @param   context         A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operands        The array that receives the decoded operands.
 *                          Refer to `ZYDIS_MAX_OPERAND_COUNT` or `ZYDIS_MAX_OPERAND_COUNT_VISIBLE`
 *                          when allocating space for the array to ensure that the buffer size is
 *                          sufficient to always fit all instruction operands.
 *                          Refer to `instruction.operand_count` or
 *                          `instruction.operand_count_visible' when allocating space for the array
 *                          to ensure that the buffer size is sufficient to fit all operands of
 *                          the given instruction.
 * @param   operand_count   The length of the `operands` array.
 *                          This argument as well limits the maximum amount of operands to decode.
 *                          If this value is `0`, no operands will be decoded and `ZYAN_NULL` will
 *                          be accepted for the `operands` argument.
 *
 * This function fails, if `operand_count` is larger than the total number of operands for the
 * given instruction (`instruction.operand_count`).
 *
 * This function is not available in MINIMAL_MODE.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderDecodeOperands(const ZydisDecoder* decoder,
    const ZydisDecoderContext* context, const ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand* operands, ZyanU8 operand_count);

/** @} */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_DECODER_H */
