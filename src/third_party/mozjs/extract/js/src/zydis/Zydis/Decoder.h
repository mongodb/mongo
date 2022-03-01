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
 * @brief   Functions for decoding instructions.
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
 * @brief   Defines the `ZydisDecoderMode` enum.
 */
typedef enum ZydisDecoderMode_
{
    /**
     * @brief   Enables minimal instruction decoding without semantic analysis.
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
     * @brief   Enables the `AMD`-branch mode.
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
     * @brief   Enables `KNC` compatibility-mode.
     *
     * `KNC` and `KNL+` chips are sharing opcodes and encodings for some mask-related instructions.
     * Enable this mode to use the old `KNC` specifications (different mnemonics, operands, ..).
     *
     * This mode is NOT enabled by default.
     */
    ZYDIS_DECODER_MODE_KNC,
    /**
     * @brief   Enables the `MPX` mode.
     *
     * The `MPX` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_MPX,
    /**
     * @brief   Enables the `CET` mode.
     *
     * The `CET` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_CET,
    /**
     * @brief   Enables the `LZCNT` mode.
     *
     * The `LZCNT` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_LZCNT,
    /**
     * @brief   Enables the `TZCNT` mode.
     *
     * The `TZCNT` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_TZCNT,
    /**
     * @brief   Enables the `WBNOINVD` mode.
     *
     * The `WBINVD` instruction is interpreted as `WBNOINVD` on ICL chips, if a `F3` prefix is
     * used.
     *
     * This mode is disabled by default.
     */
    ZYDIS_DECODER_MODE_WBNOINVD,
     /**
     * @brief   Enables the `CLDEMOTE` mode.
     *
     * The `CLDEMOTE` isa-extension reuses (overrides) some of the widenop instruction opcodes.
     *
     * This mode is enabled by default.
     */
    ZYDIS_DECODER_MODE_CLDEMOTE,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_DECODER_MODE_MAX_VALUE = ZYDIS_DECODER_MODE_CLDEMOTE,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_DECODER_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_DECODER_MODE_MAX_VALUE)
} ZydisDecoderMode;

/* ---------------------------------------------------------------------------------------------- */
/* Decoder struct                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZydisDecoder` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZydisDecoder_
{
    /**
     * @brief   The machine mode.
     */
    ZydisMachineMode machine_mode;
    /**
     * @brief   The address width.
     */
    ZydisAddressWidth address_width;
    /**
     * @brief   The decoder mode array.
     */
    ZyanBool decoder_mode[ZYDIS_DECODER_MODE_MAX_VALUE + 1];
} ZydisDecoder;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup decoder Decoder
 * @brief Functions allowing decoding of instruction bytes to a machine interpretable struct.
 * @{
 */

/**
 * @brief   Initializes the given `ZydisDecoder` instance.
 *
 * @param   decoder         A pointer to the `ZydisDecoder` instance.
 * @param   machine_mode    The machine mode.
 * @param   address_width   The address width.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderInit(ZydisDecoder* decoder, ZydisMachineMode machine_mode,
    ZydisAddressWidth address_width);

/**
 * @brief   Enables or disables the specified decoder-mode.
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
 * @brief   Decodes the instruction in the given input `buffer`.
 *
 * @param   decoder     A pointer to the `ZydisDecoder` instance.
 * @param   buffer      A pointer to the input buffer.
 * @param   length      The length of the input buffer.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct, that receives the
 *                      details about the decoded instruction.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisDecoderDecodeBuffer(const ZydisDecoder* decoder,
    const void* buffer, ZyanUSize length, ZydisDecodedInstruction* instruction);

/** @} */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_DECODER_H */
