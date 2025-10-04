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

// ReSharper disable CppClangTidyClangDiagnosticImplicitFallthrough
// ReSharper disable CppClangTidyClangDiagnosticSwitchEnum
// ReSharper disable CppClangTidyClangDiagnosticCoveredSwitchDefault

// Temporarily disabled due to a LLVM issue:
// ReSharper disable CppClangTidyBugproneNarrowingConversions

#include "zydis/Zycore/LibC.h"
#include "zydis/Zydis/Decoder.h"
#include "zydis/Zydis/Status.h"
#include "zydis/Zydis/Internal/DecoderData.h"
#include "zydis/Zydis/Internal/SharedData.h"

/* ============================================================================================== */
/* Internal enums and types                                                                       */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Decoder context                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisDecoderState` struct.
 */
typedef struct ZydisDecoderState_
{
    /**
     * A pointer to the `ZydisDecoder` instance.
     */
    const ZydisDecoder* decoder;
    /**
     * A pointer to the `ZydisDecoderContext` struct.
     */
    ZydisDecoderContext* context;
    /**
     * The input buffer.
     */
    const ZyanU8* buffer;
    /**
     * The input buffer length.
     */
    ZyanUSize buffer_len;
    /**
     * Prefix information.
     */
    struct
    {
        /**
         * Signals, if the instruction has a `LOCK` prefix (`F0`).
         *
         * This prefix originally belongs to group 1, but separating it from the other ones makes
         * parsing easier for us later.
         */
        ZyanBool has_lock;
        /**
         * The effective prefix of group 1 (either `F2` or `F3`).
         */
        ZyanU8 group1;
        /**
         * The effective prefix of group 2 (`2E`, `36`, `3E`, `26`, `64` or `65`).
         */
        ZyanU8 group2;
        /**
         * The effective segment prefix.
         */
        ZyanU8 effective_segment;
        /**
         * The prefix that should be treated as the mandatory-prefix, if the
         * current instruction needs one.
         *
         * The last `F3`/`F2` prefix has precedence over previous ones and
         * `F3`/`F2` in general have precedence over `66`.
         */
        ZyanU8 mandatory_candidate;
        /**
         * The offset of the effective `LOCK` prefix.
         */
        ZyanU8 offset_lock;
        /**
         * The offset of the effective prefix in group 1.
         */
        ZyanU8 offset_group1;
        /**
         * The offset of the effective prefix in group 2.
         */
        ZyanU8 offset_group2;
        /**
         * The offset of the operand-size override prefix (`66`).
         *
         * This is the only prefix in group 3.
         */
        ZyanU8 offset_osz_override;
        /**
         * The offset of the address-size override prefix (`67`).
         *
         * This is the only prefix in group 4.
         */
        ZyanU8 offset_asz_override;
        /**
         * The offset of the effective segment prefix.
         */
        ZyanU8 offset_segment;
        /**
         * The offset of the mandatory-candidate prefix.
         */
        ZyanU8 offset_mandatory;
        /**
         * The offset of a possible `CET` `no-lock` prefix.
         */
        ZyanI8 offset_notrack;
    } prefixes;
} ZydisDecoderState;

/* ---------------------------------------------------------------------------------------------- */
/* Register encoding                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisRegisterEncoding` enum.
 */
typedef enum ZydisRegisterEncoding_
{
    ZYDIS_REG_ENCODING_INVALID,
    /**
     * The register-id is encoded as part of the opcode (bits [3..0]).
     *
     * Possible extension by:
     * - `REX.B`
     */
    ZYDIS_REG_ENCODING_OPCODE,
    /**
     * The register-id is encoded in `modrm.reg`.
     *
     * Possible extension by:
     * - `.R`
     * - `.R'` (vector only, EVEX/MVEX)
     */
    ZYDIS_REG_ENCODING_REG,
    /**
     * The register-id is encoded in `.vvvv`.
     *
     * Possible extension by:
     * - `.v'` (vector only, EVEX/MVEX).
     */
    ZYDIS_REG_ENCODING_NDSNDD,
    /**
     * The register-id is encoded in `modrm.rm`.
     *
     * Possible extension by:
     * - `.B`
     * - `.X` (vector only, EVEX/MVEX)`
     */
    ZYDIS_REG_ENCODING_RM,
    /**
     * The register-id is encoded in `modrm.rm` or `sib.base` (if `SIB` is present).
     *
     * Possible extension by:
     * - `.B`
     */
    ZYDIS_REG_ENCODING_BASE,
    /**
     * The register-id is encoded in `sib.index`.
     *
     * Possible extension by:
     * - `.X`
     */
    ZYDIS_REG_ENCODING_INDEX,
    /**
     * The register-id is encoded in `sib.index`.
     *
     * Possible extension by:
     * - `.X`
     * - `.V'` (vector only, EVEX/MVEX)
     */
    ZYDIS_REG_ENCODING_VIDX,
    /**
     * The register-id is encoded in an additional 8-bit immediate value.
     *
     * Bits [7:4] in 64-bit mode with possible extension by bit [3] (vector only), bits [7:5] for
     * all other modes.
     */
    ZYDIS_REG_ENCODING_IS4,
    /**
     * The register-id is encoded in `EVEX.aaa/MVEX.kkk`.
     */
    ZYDIS_REG_ENCODING_MASK,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_REG_ENCODING_MAX_VALUE = ZYDIS_REG_ENCODING_MASK,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_REG_ENCODING_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_REG_ENCODING_MAX_VALUE)
} ZydisRegisterEncoding;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Input helper functions                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Reads one byte from the current read-position of the input data-source.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   value       A pointer to the memory that receives the byte from the input data-source.
 *
 * @return  A zyan status code.
 *
 * This function may fail, if the `ZYDIS_MAX_INSTRUCTION_LENGTH` limit got exceeded, or no more
 * data is available.
 */
static ZyanStatus ZydisInputPeek(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU8* value)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(value);

    if (instruction->length >= ZYDIS_MAX_INSTRUCTION_LENGTH)
    {
        return ZYDIS_STATUS_INSTRUCTION_TOO_LONG;
    }

    if (state->buffer_len > 0)
    {
        *value = state->buffer[0];
        return ZYAN_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_NO_MORE_DATA;
}

/**
 * Increases the read-position of the input data-source by one byte.
 *
 * @param   state       A pointer to the `ZydisDecoderState` instance
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 *
 * This function is supposed to get called ONLY after a successful call of `ZydisInputPeek`.
 *
 * This function increases the `length` field of the `ZydisDecodedInstruction` struct by one.
 */
static void ZydisInputSkip(ZydisDecoderState* state, ZydisDecodedInstruction* instruction)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(instruction->length < ZYDIS_MAX_INSTRUCTION_LENGTH);

    ++instruction->length;
    ++state->buffer;
    --state->buffer_len;
}

/**
 * Reads one byte from the current read-position of the input data-source and increases
 *          the read-position by one byte afterwards.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   value       A pointer to the memory that receives the byte from the input data-source.
 *
 * @return  A zyan status code.
 *
 * This function acts like a subsequent call of `ZydisInputPeek` and `ZydisInputSkip`.
 */
static ZyanStatus ZydisInputNext(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU8* value)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(value);

    if (instruction->length >= ZYDIS_MAX_INSTRUCTION_LENGTH)
    {
        return ZYDIS_STATUS_INSTRUCTION_TOO_LONG;
    }

    if (state->buffer_len > 0)
    {
        *value = state->buffer++[0];
        ++instruction->length;
        --state->buffer_len;
        return ZYAN_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_NO_MORE_DATA;
}

/**
 * Reads a variable amount of bytes from the current read-position of the input
 *          data-source and increases the read-position by specified amount of bytes afterwards.
 *
 * @param   state           A pointer to the `ZydisDecoderState` struct.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   value           A pointer to the memory that receives the byte from the input
 *                          data-source.
 * @param   number_of_bytes The number of bytes to read from the input data-source.
 *
 * @return  A zyan status code.
 *
 * This function acts like a subsequent call of `ZydisInputPeek` and `ZydisInputSkip`.
 */
static ZyanStatus ZydisInputNextBytes(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU8* value, ZyanU8 number_of_bytes)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(value);

    if (instruction->length + number_of_bytes > ZYDIS_MAX_INSTRUCTION_LENGTH)
    {
        return ZYDIS_STATUS_INSTRUCTION_TOO_LONG;
    }

    if (state->buffer_len >= number_of_bytes)
    {
        instruction->length += number_of_bytes;

        ZYAN_MEMCPY(value, state->buffer, number_of_bytes);
        state->buffer += number_of_bytes;
        state->buffer_len -= number_of_bytes;

        return ZYAN_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_NO_MORE_DATA;
}

/* ---------------------------------------------------------------------------------------------- */
/* Decode functions                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Decodes the `REX`-prefix.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   data        The `REX` byte.
 */
static void ZydisDecodeREX(ZydisDecoderContext* context, ZydisDecodedInstruction* instruction,
    ZyanU8 data)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT((data & 0xF0) == 0x40);

    instruction->attributes |= ZYDIS_ATTRIB_HAS_REX;
    instruction->raw.rex.W   = (data >> 3) & 0x01;
    instruction->raw.rex.R   = (data >> 2) & 0x01;
    instruction->raw.rex.X   = (data >> 1) & 0x01;
    instruction->raw.rex.B   = (data >> 0) & 0x01;

    // Update internal fields
    context->vector_unified.W = instruction->raw.rex.W;
    context->vector_unified.R = instruction->raw.rex.R;
    context->vector_unified.X = instruction->raw.rex.X;
    context->vector_unified.B = instruction->raw.rex.B;
}

/**
 * Decodes the `XOP`-prefix.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   data        The `XOP` bytes.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeXOP(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZyanU8 data[3])
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(data[0] == 0x8F);
    ZYAN_ASSERT(((data[1] >> 0) & 0x1F) >= 8);
    ZYAN_ASSERT(instruction->raw.xop.offset == instruction->length - 3);

    if (instruction->machine_mode == ZYDIS_MACHINE_MODE_REAL_16)
    {
        // XOP is invalid in 16-bit real mode
        return ZYDIS_STATUS_DECODING_ERROR;
    }

    instruction->attributes |= ZYDIS_ATTRIB_HAS_XOP;
    instruction->raw.xop.R       = (data[1] >> 7) & 0x01;
    instruction->raw.xop.X       = (data[1] >> 6) & 0x01;
    instruction->raw.xop.B       = (data[1] >> 5) & 0x01;
    instruction->raw.xop.m_mmmm  = (data[1] >> 0) & 0x1F;

    if ((instruction->raw.xop.m_mmmm < 0x08) || (instruction->raw.xop.m_mmmm > 0x0A))
    {
        // Invalid according to the AMD documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    instruction->raw.xop.W    = (data[2] >> 7) & 0x01;
    instruction->raw.xop.vvvv = (data[2] >> 3) & 0x0F;
    instruction->raw.xop.L    = (data[2] >> 2) & 0x01;
    instruction->raw.xop.pp   = (data[2] >> 0) & 0x03;

    // Update internal fields
    context->vector_unified.W    = instruction->raw.xop.W;
    context->vector_unified.R    = 0x01 & ~instruction->raw.xop.R;
    context->vector_unified.X    = 0x01 & ~instruction->raw.xop.X;
    context->vector_unified.B    = 0x01 & ~instruction->raw.xop.B;
    context->vector_unified.L    = instruction->raw.xop.L;
    context->vector_unified.LL   = instruction->raw.xop.L;
    context->vector_unified.vvvv = (0x0F & ~instruction->raw.xop.vvvv);

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Decodes the `VEX`-prefix.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   data        The `VEX` bytes.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeVEX(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZyanU8 data[3])
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT((data[0] == 0xC4) || (data[0] == 0xC5));

    if (instruction->machine_mode == ZYDIS_MACHINE_MODE_REAL_16)
    {
        // VEX is invalid in 16-bit real mode
        return ZYDIS_STATUS_DECODING_ERROR;
    }

    instruction->attributes |= ZYDIS_ATTRIB_HAS_VEX;
    switch (data[0])
    {
    case 0xC4:
        ZYAN_ASSERT(instruction->raw.vex.offset == instruction->length - 3);
        instruction->raw.vex.size    = 3;
        instruction->raw.vex.R       = (data[1] >> 7) & 0x01;
        instruction->raw.vex.X       = (data[1] >> 6) & 0x01;
        instruction->raw.vex.B       = (data[1] >> 5) & 0x01;
        instruction->raw.vex.m_mmmm  = (data[1] >> 0) & 0x1F;
        instruction->raw.vex.W       = (data[2] >> 7) & 0x01;
        instruction->raw.vex.vvvv    = (data[2] >> 3) & 0x0F;
        instruction->raw.vex.L       = (data[2] >> 2) & 0x01;
        instruction->raw.vex.pp      = (data[2] >> 0) & 0x03;
        break;
    case 0xC5:
        ZYAN_ASSERT(instruction->raw.vex.offset == instruction->length - 2);
        instruction->raw.vex.size    = 2;
        instruction->raw.vex.R       = (data[1] >> 7) & 0x01;
        instruction->raw.vex.X       = 1;
        instruction->raw.vex.B       = 1;
        instruction->raw.vex.m_mmmm  = 1;
        instruction->raw.vex.W       = 0;
        instruction->raw.vex.vvvv    = (data[1] >> 3) & 0x0F;
        instruction->raw.vex.L       = (data[1] >> 2) & 0x01;
        instruction->raw.vex.pp      = (data[1] >> 0) & 0x03;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    // Map 0 is only valid for some KNC instructions
#ifdef ZYDIS_DISABLE_KNC
    if ((instruction->raw.vex.m_mmmm == 0) || (instruction->raw.vex.m_mmmm > 0x03))
#else
    if (instruction->raw.vex.m_mmmm > 0x03)
#endif
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    // Update internal fields
    context->vector_unified.W    = instruction->raw.vex.W;
    context->vector_unified.R    = 0x01 & ~instruction->raw.vex.R;
    context->vector_unified.X    = 0x01 & ~instruction->raw.vex.X;
    context->vector_unified.B    = 0x01 & ~instruction->raw.vex.B;
    context->vector_unified.L    = instruction->raw.vex.L;
    context->vector_unified.LL   = instruction->raw.vex.L;
    context->vector_unified.vvvv = (0x0F & ~instruction->raw.vex.vvvv);

    return ZYAN_STATUS_SUCCESS;
}

#ifndef ZYDIS_DISABLE_AVX512
/**
 * Decodes the `EVEX`-prefix.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   data        The `EVEX` bytes.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeEVEX(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZyanU8 data[4])
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(data[0] == 0x62);
    ZYAN_ASSERT(instruction->raw.evex.offset == instruction->length - 4);

    if (instruction->machine_mode == ZYDIS_MACHINE_MODE_REAL_16)
    {
        // EVEX is invalid in 16-bit real mode
        return ZYDIS_STATUS_DECODING_ERROR;
    }

    instruction->attributes |= ZYDIS_ATTRIB_HAS_EVEX;
    instruction->raw.evex.R         = (data[1] >> 7) & 0x01;
    instruction->raw.evex.X         = (data[1] >> 6) & 0x01;
    instruction->raw.evex.B         = (data[1] >> 5) & 0x01;
    instruction->raw.evex.R2        = (data[1] >> 4) & 0x01;

    if (data[1] & 0x08)
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }

    instruction->raw.evex.mmm       = (data[1] >> 0) & 0x07;

    if ((instruction->raw.evex.mmm == 0x00) ||
        (instruction->raw.evex.mmm == 0x04) ||
        (instruction->raw.evex.mmm == 0x07))
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    instruction->raw.evex.W         = (data[2] >> 7) & 0x01;
    instruction->raw.evex.vvvv      = (data[2] >> 3) & 0x0F;

    ZYAN_ASSERT(((data[2] >> 2) & 0x01) == 0x01);

    instruction->raw.evex.pp        = (data[2] >> 0) & 0x03;
    instruction->raw.evex.z         = (data[3] >> 7) & 0x01;
    instruction->raw.evex.L2        = (data[3] >> 6) & 0x01;
    instruction->raw.evex.L         = (data[3] >> 5) & 0x01;
    instruction->raw.evex.b         = (data[3] >> 4) & 0x01;
    instruction->raw.evex.V2        = (data[3] >> 3) & 0x01;

    if (!instruction->raw.evex.V2 &&
        (instruction->machine_mode != ZYDIS_MACHINE_MODE_LONG_64))
    {
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }

    instruction->raw.evex.aaa       = (data[3] >> 0) & 0x07;

    if (instruction->raw.evex.z && !instruction->raw.evex.aaa)
    {
        return ZYDIS_STATUS_INVALID_MASK; // TODO: Dedicated status code
    }

    // Update internal fields
    context->vector_unified.W    = instruction->raw.evex.W;
    context->vector_unified.R    = 0x01 & ~instruction->raw.evex.R;
    context->vector_unified.X    = 0x01 & ~instruction->raw.evex.X;
    context->vector_unified.B    = 0x01 & ~instruction->raw.evex.B;
    context->vector_unified.LL   = (data[3] >> 5) & 0x03;
    context->vector_unified.R2   = 0x01 & ~instruction->raw.evex.R2;
    context->vector_unified.V2   = 0x01 & ~instruction->raw.evex.V2;
    context->vector_unified.vvvv = 0x0F & ~instruction->raw.evex.vvvv;
    context->vector_unified.mask = instruction->raw.evex.aaa;

    if (!instruction->raw.evex.V2 && (instruction->machine_mode != ZYDIS_MACHINE_MODE_LONG_64))
    {
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }
    if (!instruction->raw.evex.b && (context->vector_unified.LL == 3))
    {
        // LL = 3 is only valid for instructions with embedded rounding control
        return ZYDIS_STATUS_MALFORMED_EVEX;
    }

    return ZYAN_STATUS_SUCCESS;
}
#endif

#ifndef ZYDIS_DISABLE_KNC
/**
 * Decodes the `MVEX`-prefix.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   data        The `MVEX` bytes.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeMVEX(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZyanU8 data[4])
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(data[0] == 0x62);
    ZYAN_ASSERT(instruction->raw.mvex.offset == instruction->length - 4);

    if (instruction->machine_mode != ZYDIS_MACHINE_MODE_LONG_64)
    {
        // MVEX is only valid in 64-bit mode
        return ZYDIS_STATUS_DECODING_ERROR;
    }

    instruction->attributes |= ZYDIS_ATTRIB_HAS_MVEX;
    instruction->raw.mvex.R    = (data[1] >> 7) & 0x01;
    instruction->raw.mvex.X    = (data[1] >> 6) & 0x01;
    instruction->raw.mvex.B    = (data[1] >> 5) & 0x01;
    instruction->raw.mvex.R2   = (data[1] >> 4) & 0x01;
    instruction->raw.mvex.mmmm = (data[1] >> 0) & 0x0F;

    if (instruction->raw.mvex.mmmm > 0x03)
    {
        // Invalid according to the intel documentation
        return ZYDIS_STATUS_INVALID_MAP;
    }

    instruction->raw.mvex.W    = (data[2] >> 7) & 0x01;
    instruction->raw.mvex.vvvv = (data[2] >> 3) & 0x0F;

    ZYAN_ASSERT(((data[2] >> 2) & 0x01) == 0x00);

    instruction->raw.mvex.pp   = (data[2] >> 0) & 0x03;
    instruction->raw.mvex.E    = (data[3] >> 7) & 0x01;
    instruction->raw.mvex.SSS  = (data[3] >> 4) & 0x07;
    instruction->raw.mvex.V2   = (data[3] >> 3) & 0x01;
    instruction->raw.mvex.kkk  = (data[3] >> 0) & 0x07;

    // Update internal fields
    context->vector_unified.W    = instruction->raw.mvex.W;
    context->vector_unified.R    = 0x01 & ~instruction->raw.mvex.R;
    context->vector_unified.X    = 0x01 & ~instruction->raw.mvex.X;
    context->vector_unified.B    = 0x01 & ~instruction->raw.mvex.B;
    context->vector_unified.R2   = 0x01 & ~instruction->raw.mvex.R2;
    context->vector_unified.V2   = 0x01 & ~instruction->raw.mvex.V2;
    context->vector_unified.LL   = 2;
    context->vector_unified.vvvv = 0x0F & ~instruction->raw.mvex.vvvv;
    context->vector_unified.mask = instruction->raw.mvex.kkk;

    return ZYAN_STATUS_SUCCESS;
}
#endif

/**
 * Decodes the `ModRM`-byte.
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   data        The `ModRM` byte.
 */
static void ZydisDecodeModRM(ZydisDecodedInstruction* instruction, ZyanU8 data)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(!(instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM));
    ZYAN_ASSERT(instruction->raw.modrm.offset == instruction->length - 1);

    instruction->attributes   |= ZYDIS_ATTRIB_HAS_MODRM;
    instruction->raw.modrm.mod = (data >> 6) & 0x03;
    instruction->raw.modrm.reg = (data >> 3) & 0x07;
    instruction->raw.modrm.rm  = (data >> 0) & 0x07;
}

/**
 * Decodes the `SIB`-byte.
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct
 * @param   data        The `SIB` byte.
 */
static void ZydisDecodeSIB(ZydisDecodedInstruction* instruction, ZyanU8 data)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM);
    ZYAN_ASSERT(instruction->raw.modrm.rm == 4);
    ZYAN_ASSERT(!(instruction->attributes & ZYDIS_ATTRIB_HAS_SIB));
    ZYAN_ASSERT(instruction->raw.sib.offset == instruction->length - 1);

    instruction->attributes    |= ZYDIS_ATTRIB_HAS_SIB;
    instruction->raw.sib.scale = (data >> 6) & 0x03;
    instruction->raw.sib.index = (data >> 3) & 0x07;
    instruction->raw.sib.base  = (data >> 0) & 0x07;
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * Reads a displacement value.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   size        The physical size of the displacement value.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisReadDisplacement(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU8 size)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(instruction->raw.disp.size == 0);

    instruction->raw.disp.size = size;
    instruction->raw.disp.offset = instruction->length;

    switch (size)
    {
    case 8:
    {
        ZyanU8 value;
        ZYAN_CHECK(ZydisInputNext(state, instruction, &value));
        instruction->raw.disp.value = *(ZyanI8*)&value;
        break;
    }
    case 16:
    {
        ZyanU16 value;
        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, (ZyanU8*)&value, 2));
        instruction->raw.disp.value = *(ZyanI16*)&value;
        break;
    }
    case 32:
    {
        ZyanU32 value;
        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, (ZyanU8*)&value, 4));
        instruction->raw.disp.value = *(ZyanI32*)&value;
        break;
    }
    case 64:
    {
        ZyanU64 value;
        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, (ZyanU8*)&value, 8));
        instruction->raw.disp.value = *(ZyanI64*)&value;
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }

    // TODO: Fix endianess on big-endian systems

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Reads an immediate value.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   id          The immediate id (either `0` or `1`).
 * @param   size        The physical size of the immediate value.
 * @param   is_signed   Signals, if the immediate value is signed.
 * @param   is_relative Signals, if the immediate value is a relative offset.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisReadImmediate(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU8 id, ZyanU8 size, ZyanBool is_signed,
    ZyanBool is_relative)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT((id == 0) || (id == 1));
    ZYAN_ASSERT(is_signed || !is_relative);
    ZYAN_ASSERT(instruction->raw.imm[id].size == 0);

    instruction->raw.imm[id].size = size;
    instruction->raw.imm[id].offset = instruction->length;
    instruction->raw.imm[id].is_signed = is_signed;
    instruction->raw.imm[id].is_relative = is_relative;
    switch (size)
    {
    case 8:
    {
        ZyanU8 value;
        ZYAN_CHECK(ZydisInputNext(state, instruction, &value));
        if (is_signed)
        {
            instruction->raw.imm[id].value.s = (ZyanI8)value;
        } else
        {
            instruction->raw.imm[id].value.u = value;
        }
        break;
    }
    case 16:
    {
        ZyanU16 value;
        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, (ZyanU8*)&value, 2));
        if (is_signed)
        {
            instruction->raw.imm[id].value.s = (ZyanI16)value;
        } else
        {
            instruction->raw.imm[id].value.u = value;
        }
        break;
    }
    case 32:
    {
        ZyanU32 value;
        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, (ZyanU8*)&value, 4));
        if (is_signed)
        {
            instruction->raw.imm[id].value.s = (ZyanI32)value;
        } else
        {
            instruction->raw.imm[id].value.u = value;
        }
        break;
    }
    case 64:
    {
        ZyanU64 value;
        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, (ZyanU8*)&value, 8));
        if (is_signed)
        {
            instruction->raw.imm[id].value.s = (ZyanI64)value;
        } else
        {
            instruction->raw.imm[id].value.u = value;
        }
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }

    // TODO: Fix endianess on big-endian systems

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Semantic instruction decoding                                                                  */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Calculates the register-id for a specific register-encoding and register-class.
 *
 * @param   context         A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction     A pointer to the ` ZydisDecodedInstruction` struct.
 * @param   encoding        The register-encoding.
 * @param   register_class  The register-class.
 *
 * @return  A zyan status code.
 *
 * This function calculates the register-id by combining different fields and flags of previously
 * decoded structs.
 */
static ZyanU8 ZydisCalcRegisterId(const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZydisRegisterEncoding encoding,
    ZydisRegisterClass register_class)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);

    // TODO: Combine OPCODE and IS4 in `ZydisPopulateRegisterIds` and get rid of this
    // TODO: function entirely

    switch (encoding)
    {
    case ZYDIS_REG_ENCODING_REG:
        return context->reg_info.id_reg;
    case ZYDIS_REG_ENCODING_NDSNDD:
        return context->reg_info.id_ndsndd;
    case ZYDIS_REG_ENCODING_RM:
        return context->reg_info.id_rm;
    case ZYDIS_REG_ENCODING_BASE:
        return context->reg_info.id_base;
    case ZYDIS_REG_ENCODING_INDEX:
    case ZYDIS_REG_ENCODING_VIDX:
        return context->reg_info.id_index;
    case ZYDIS_REG_ENCODING_OPCODE:
    {
        ZYAN_ASSERT((register_class == ZYDIS_REGCLASS_GPR8) ||
                    (register_class == ZYDIS_REGCLASS_GPR16) ||
                    (register_class == ZYDIS_REGCLASS_GPR32) ||
                    (register_class == ZYDIS_REGCLASS_GPR64));
        ZyanU8 value = (instruction->opcode & 0x0F);
        if (value > 7)
        {
            value = value - 8;
        }
        if (instruction->machine_mode != ZYDIS_MACHINE_MODE_LONG_64)
        {
            return value;
        }
        return value | (context->vector_unified.B << 3);
    }
    case ZYDIS_REG_ENCODING_IS4:
    {
        if (instruction->machine_mode != ZYDIS_MACHINE_MODE_LONG_64)
        {
            return (instruction->raw.imm[0].value.u >> 4) & 0x07;
        }
        ZyanU8 value = (instruction->raw.imm[0].value.u >> 4) & 0x0F;
        // We have to check the instruction-encoding, because the extension by bit [3] is only
        // valid for EVEX and MVEX instructions
        if ((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
            (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX))
        {
            switch (register_class)
            {
            case ZYDIS_REGCLASS_XMM:
            case ZYDIS_REGCLASS_YMM:
            case ZYDIS_REGCLASS_ZMM:
                value |= ((instruction->raw.imm[0].value.u & 0x08) << 1);
            default:
                break;
            }
        }
        return value;
    }
    case ZYDIS_REG_ENCODING_MASK:
        return context->vector_unified.mask;
    default:
        ZYAN_UNREACHABLE;
    }
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Sets the operand-size and element-specific information for the given operand.
 *
 * @param   context         A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand         A pointer to the `ZydisDecodedOperand` struct.
 * @param   definition      A pointer to the `ZydisOperandDefinition` struct.
 */
static void ZydisSetOperandSizeAndElementInfo(const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operand,
    const ZydisOperandDefinition* definition)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(operand);
    ZYAN_ASSERT(definition);

    // Operand size
    switch (operand->type)
    {
    case ZYDIS_OPERAND_TYPE_REGISTER:
    {
        if (definition->size[context->eosz_index])
        {
            operand->size = definition->size[context->eosz_index] * 8;
        } else
        {
            operand->size = ZydisRegisterGetWidth(instruction->machine_mode,
                operand->reg.value);
        }
        operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
        operand->element_size = operand->size;
        break;
    }
    case ZYDIS_OPERAND_TYPE_MEMORY:
        switch (instruction->encoding)
        {
        case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
        case ZYDIS_INSTRUCTION_ENCODING_XOP:
        case ZYDIS_INSTRUCTION_ENCODING_VEX:
            if (operand->mem.type == ZYDIS_MEMOP_TYPE_AGEN)
            {
                ZYAN_ASSERT(definition->size[context->eosz_index] == 0);
                operand->size = instruction->address_width;
                operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
            } else
            {
                ZYAN_ASSERT(definition->size[context->eosz_index] ||
                    (instruction->meta.category == ZYDIS_CATEGORY_AMX_TILE));
                operand->size = definition->size[context->eosz_index] * 8;
            }
            break;
        case ZYDIS_INSTRUCTION_ENCODING_EVEX:
#ifndef ZYDIS_DISABLE_AVX512
            if (definition->size[context->eosz_index])
            {
                // Operand size is hardcoded
                operand->size = definition->size[context->eosz_index] * 8;
            } else
            {
                // Operand size depends on the tuple-type, the element-size and the number of
                // elements
                ZYAN_ASSERT(instruction->avx.vector_length);
                ZYAN_ASSERT(context->evex.element_size);
                switch (context->evex.tuple_type)
                {
                case ZYDIS_TUPLETYPE_FV:
                    if (instruction->avx.broadcast.mode)
                    {
                        operand->size = context->evex.element_size;
                    } else
                    {
                        operand->size = instruction->avx.vector_length;
                    }
                    break;
                case ZYDIS_TUPLETYPE_HV:
                    if (instruction->avx.broadcast.mode)
                    {
                        operand->size = context->evex.element_size;
                    } else
                    {
                        operand->size = (ZyanU16)instruction->avx.vector_length / 2;
                    }
                    break;
                case ZYDIS_TUPLETYPE_QUARTER:
                    if (instruction->avx.broadcast.mode)
                    {
                        operand->size = context->evex.element_size;
                    }
                    else
                    {
                        operand->size = (ZyanU16)instruction->avx.vector_length / 4;
                    }
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
            }
            ZYAN_ASSERT(operand->size);
#else
            ZYAN_UNREACHABLE;
#endif
            break;
        case ZYDIS_INSTRUCTION_ENCODING_MVEX:
#ifndef ZYDIS_DISABLE_KNC
            if (definition->size[context->eosz_index])
            {
                // Operand size is hardcoded
                operand->size = definition->size[context->eosz_index] * 8;
            } else
            {
                ZYAN_ASSERT(definition->element_type == ZYDIS_IELEMENT_TYPE_VARIABLE);
                ZYAN_ASSERT(instruction->avx.vector_length == 512);

                switch (instruction->avx.conversion.mode)
                {
                case ZYDIS_CONVERSION_MODE_INVALID:
                    operand->size = 512;
                    switch (context->mvex.functionality)
                    {
                    case ZYDIS_MVEX_FUNC_SF_32:
                    case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
                    case ZYDIS_MVEX_FUNC_UF_32:
                    case ZYDIS_MVEX_FUNC_DF_32:
                        operand->element_type = ZYDIS_ELEMENT_TYPE_FLOAT32;
                        operand->element_size = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SF_32_BCST:
                        operand->size = 256;
                        operand->element_type = ZYDIS_ELEMENT_TYPE_FLOAT32;
                        operand->element_size = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SI_32:
                    case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
                    case ZYDIS_MVEX_FUNC_UI_32:
                    case ZYDIS_MVEX_FUNC_DI_32:
                        operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
                        operand->element_size = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SI_32_BCST:
                        operand->size = 256;
                        operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
                        operand->element_size = 32;
                        break;
                    case ZYDIS_MVEX_FUNC_SF_64:
                    case ZYDIS_MVEX_FUNC_UF_64:
                    case ZYDIS_MVEX_FUNC_DF_64:
                        operand->element_type = ZYDIS_ELEMENT_TYPE_FLOAT64;
                        operand->element_size = 64;
                        break;
                    case ZYDIS_MVEX_FUNC_SI_64:
                    case ZYDIS_MVEX_FUNC_UI_64:
                    case ZYDIS_MVEX_FUNC_DI_64:
                        operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
                        operand->element_size = 64;
                        break;
                    default:
                        ZYAN_UNREACHABLE;
                    }
                    break;
                case ZYDIS_CONVERSION_MODE_FLOAT16:
                    operand->size = 256;
                    operand->element_type = ZYDIS_ELEMENT_TYPE_FLOAT16;
                    operand->element_size = 16;
                    break;
                case ZYDIS_CONVERSION_MODE_SINT16:
                    operand->size = 256;
                    operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
                    operand->element_size = 16;
                    break;
                case ZYDIS_CONVERSION_MODE_UINT16:
                    operand->size = 256;
                    operand->element_type = ZYDIS_ELEMENT_TYPE_UINT;
                    operand->element_size = 16;
                    break;
                case ZYDIS_CONVERSION_MODE_SINT8:
                    operand->size = 128;
                    operand->element_type = ZYDIS_ELEMENT_TYPE_INT;
                    operand->element_size = 8;
                    break;
                case ZYDIS_CONVERSION_MODE_UINT8:
                    operand->size = 128;
                    operand->element_type = ZYDIS_ELEMENT_TYPE_UINT;
                    operand->element_size = 8;
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }

                switch (instruction->avx.broadcast.mode)
                {
                case ZYDIS_BROADCAST_MODE_INVALID:
                    // Nothing to do here
                    break;
                case ZYDIS_BROADCAST_MODE_1_TO_8:
                case ZYDIS_BROADCAST_MODE_1_TO_16:
                    operand->size = operand->element_size;
                    break;
                case ZYDIS_BROADCAST_MODE_4_TO_8:
                case ZYDIS_BROADCAST_MODE_4_TO_16:
                    operand->size = operand->element_size * 4;
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
            }
#else
            ZYAN_UNREACHABLE;
#endif
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        break;
    case ZYDIS_OPERAND_TYPE_POINTER:
        ZYAN_ASSERT((instruction->raw.imm[0].size == 16) ||
                    (instruction->raw.imm[0].size == 32));
        ZYAN_ASSERT( instruction->raw.imm[1].size == 16);
        operand->size = instruction->raw.imm[0].size + instruction->raw.imm[1].size;
        break;
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        operand->size = definition->size[context->eosz_index] * 8;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    // Element-type and -size
    if (definition->element_type && (definition->element_type != ZYDIS_IELEMENT_TYPE_VARIABLE))
    {
        ZydisGetElementInfo(definition->element_type, &operand->element_type,
            &operand->element_size);
        if (!operand->element_size)
        {
            // The element size is the same as the operand size. This is used for single element
            // scaling operands
            operand->element_size = operand->size;
        }
    }

    // Element count
    if (operand->element_size && operand->size && (operand->element_type != ZYDIS_ELEMENT_TYPE_CC))
    {
        operand->element_count = operand->size / operand->element_size;
    } else
    {
        operand->element_count = 1;
    }
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Decodes an register-operand.
 *
 * @param   instruction      A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand          A pointer to the `ZydisDecodedOperand` struct.
 * @param   register_class   The register class.
 * @param   register_id      The register id.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeOperandRegister(const ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand* operand, ZydisRegisterClass register_class, ZyanU8 register_id)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(operand);

    operand->type = ZYDIS_OPERAND_TYPE_REGISTER;

    if (register_class == ZYDIS_REGCLASS_GPR8)
    {
        if ((instruction->attributes & ZYDIS_ATTRIB_HAS_REX) && (register_id >= 4))
        {
            operand->reg.value = ZYDIS_REGISTER_SPL + (register_id - 4);
        } else
        {
            operand->reg.value = ZYDIS_REGISTER_AL + register_id;
        }
    } else
    {
        operand->reg.value = ZydisRegisterEncode(register_class, register_id);
        ZYAN_ASSERT(operand->reg.value);
        /*if (!operand->reg.value)
        {
            return ZYAN_STATUS_BAD_REGISTER;
        }*/
    }

    return ZYAN_STATUS_SUCCESS;
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Decodes a memory operand.
 *
 * @param   context             A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction         A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand             A pointer to the `ZydisDecodedOperand` struct.
 * @param   vidx_register_class The register-class to use as the index register-class for
 *                              instructions with `VSIB` addressing.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeOperandMemory(const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operand,
    ZydisRegisterClass vidx_register_class)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(operand);
    ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM);
    ZYAN_ASSERT(instruction->raw.modrm.mod != 3);
    ZYAN_ASSERT(!vidx_register_class || ((instruction->raw.modrm.rm == 4) &&
        ((instruction->address_width == 32) || (instruction->address_width == 64))));

    operand->type = ZYDIS_OPERAND_TYPE_MEMORY;
    operand->mem.type = ZYDIS_MEMOP_TYPE_MEM;

    const ZyanU8 modrm_rm = instruction->raw.modrm.rm;
    ZyanU8 displacement_size = 0;
    switch (instruction->address_width)
    {
    case 16:
    {
        static const ZydisRegister bases[] =
        {
            ZYDIS_REGISTER_BX,   ZYDIS_REGISTER_BX,   ZYDIS_REGISTER_BP,   ZYDIS_REGISTER_BP,
            ZYDIS_REGISTER_SI,   ZYDIS_REGISTER_DI,   ZYDIS_REGISTER_BP,   ZYDIS_REGISTER_BX
        };
        static const ZydisRegister indices[] =
        {
            ZYDIS_REGISTER_SI,   ZYDIS_REGISTER_DI,   ZYDIS_REGISTER_SI,   ZYDIS_REGISTER_DI,
            ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE
        };
        operand->mem.base = bases[modrm_rm];
        operand->mem.index = indices[modrm_rm];
        operand->mem.scale = (operand->mem.index == ZYDIS_REGISTER_NONE) ? 0 : 1;
        switch (instruction->raw.modrm.mod)
        {
        case 0:
            if (modrm_rm == 6)
            {
                displacement_size = 16;
                operand->mem.base = ZYDIS_REGISTER_NONE;
            }
            break;
        case 1:
            displacement_size = 8;
            break;
        case 2:
            displacement_size = 16;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        break;
    }
    case 32:
    {
        operand->mem.base = ZYDIS_REGISTER_EAX + ZydisCalcRegisterId(context, instruction,
            ZYDIS_REG_ENCODING_BASE, ZYDIS_REGCLASS_GPR32);
        switch (instruction->raw.modrm.mod)
        {
        case 0:
            if (modrm_rm == 5)
            {
                if (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
                {
                    operand->mem.base = ZYDIS_REGISTER_EIP;
                } else
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                }
                displacement_size = 32;
            }
            break;
        case 1:
            displacement_size = 8;
            break;
        case 2:
            displacement_size = 32;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        if (modrm_rm == 4)
        {
            ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_SIB);
            operand->mem.index =
                ZydisRegisterEncode(vidx_register_class ? vidx_register_class : ZYDIS_REGCLASS_GPR32,
                    ZydisCalcRegisterId(context, instruction,
                        vidx_register_class ? ZYDIS_REG_ENCODING_VIDX : ZYDIS_REG_ENCODING_INDEX,
                        vidx_register_class ? vidx_register_class : ZYDIS_REGCLASS_GPR32));
            operand->mem.scale = (1 << instruction->raw.sib.scale);
            if (operand->mem.index == ZYDIS_REGISTER_ESP)
            {
                operand->mem.index = ZYDIS_REGISTER_NONE;
                operand->mem.scale = 0;
            }
            if (operand->mem.base == ZYDIS_REGISTER_EBP)
            {
                if (instruction->raw.modrm.mod == 0)
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                }
                displacement_size = (instruction->raw.modrm.mod == 1) ? 8 : 32;
            }
        } else
        {
            operand->mem.index = ZYDIS_REGISTER_NONE;
            operand->mem.scale = 0;
        }
        break;
    }
    case 64:
    {
        operand->mem.base = ZYDIS_REGISTER_RAX + ZydisCalcRegisterId(context, instruction,
            ZYDIS_REG_ENCODING_BASE, ZYDIS_REGCLASS_GPR64);
        switch (instruction->raw.modrm.mod)
        {
        case 0:
            if (modrm_rm == 5)
            {
                if (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
                {
                    operand->mem.base = ZYDIS_REGISTER_RIP;
                } else
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                }
                displacement_size = 32;
            }
            break;
        case 1:
            displacement_size = 8;
            break;
        case 2:
            displacement_size = 32;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        if ((modrm_rm & 0x07) == 4)
        {
            ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_SIB);
            operand->mem.index =
                ZydisRegisterEncode(vidx_register_class ? vidx_register_class : ZYDIS_REGCLASS_GPR64,
                    ZydisCalcRegisterId(context, instruction,
                        vidx_register_class ? ZYDIS_REG_ENCODING_VIDX : ZYDIS_REG_ENCODING_INDEX,
                        vidx_register_class ? vidx_register_class : ZYDIS_REGCLASS_GPR64));
            operand->mem.scale = (1 << instruction->raw.sib.scale);
            if (operand->mem.index == ZYDIS_REGISTER_RSP)
            {
                operand->mem.index = ZYDIS_REGISTER_NONE;
                operand->mem.scale = 0;
            }
            if ((operand->mem.base == ZYDIS_REGISTER_RBP) ||
                (operand->mem.base == ZYDIS_REGISTER_R13))
            {
                if (instruction->raw.modrm.mod == 0)
                {
                    operand->mem.base = ZYDIS_REGISTER_NONE;
                }
                displacement_size = (instruction->raw.modrm.mod == 1) ? 8 : 32;
            }
        } else
        {
            operand->mem.index = ZYDIS_REGISTER_NONE;
            operand->mem.scale = 0;
        }
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }
    if (displacement_size)
    {
        ZYAN_ASSERT(instruction->raw.disp.size == displacement_size);
        operand->mem.disp.has_displacement = ZYAN_TRUE;
        operand->mem.disp.value = instruction->raw.disp.value;
    }
    return ZYAN_STATUS_SUCCESS;
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Decodes an implicit register operand.
 *
 * @param   decoder         A pointer to the `ZydisDecoder` instance.
 * @param   context         A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand         A pointer to the `ZydisDecodedOperand` struct.
 * @param   definition      A pointer to the `ZydisOperandDefinition` struct.
 */
static void ZydisDecodeOperandImplicitRegister(const ZydisDecoder* decoder,
    const ZydisDecoderContext* context, const ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand* operand, const ZydisOperandDefinition* definition)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(operand);
    ZYAN_ASSERT(definition);

    operand->type = ZYDIS_OPERAND_TYPE_REGISTER;

    switch (definition->op.reg.type)
    {
    case ZYDIS_IMPLREG_TYPE_STATIC:
        operand->reg.value = definition->op.reg.reg.reg;
        break;
    case ZYDIS_IMPLREG_TYPE_GPR_OSZ:
    {
        static const ZydisRegisterClass lookup[3] =
        {
            ZYDIS_REGCLASS_GPR16,
            ZYDIS_REGCLASS_GPR32,
            ZYDIS_REGCLASS_GPR64
        };
        operand->reg.value =
            ZydisRegisterEncode(lookup[context->eosz_index], definition->op.reg.reg.id);
        break;
    }
    case ZYDIS_IMPLREG_TYPE_GPR_ASZ:
        operand->reg.value = ZydisRegisterEncode(
            (instruction->address_width    == 16) ? ZYDIS_REGCLASS_GPR16  :
            (instruction->address_width    == 32) ? ZYDIS_REGCLASS_GPR32  : ZYDIS_REGCLASS_GPR64,
            definition->op.reg.reg.id);
        break;
    case ZYDIS_IMPLREG_TYPE_IP_ASZ:
        operand->reg.value =
            (instruction->address_width    == 16) ? ZYDIS_REGISTER_IP     :
            (instruction->address_width    == 32) ? ZYDIS_REGISTER_EIP    : ZYDIS_REGISTER_RIP;
        break;
    case ZYDIS_IMPLREG_TYPE_GPR_SSZ:
        operand->reg.value = ZydisRegisterEncode(
            (decoder->stack_width == ZYDIS_STACK_WIDTH_16) ? ZYDIS_REGCLASS_GPR16 :
            (decoder->stack_width == ZYDIS_STACK_WIDTH_32) ? ZYDIS_REGCLASS_GPR32 :
                                                             ZYDIS_REGCLASS_GPR64,
            definition->op.reg.reg.id);
        break;
    case ZYDIS_IMPLREG_TYPE_IP_SSZ:
        operand->reg.value =
            (decoder->stack_width == ZYDIS_STACK_WIDTH_16) ? ZYDIS_REGISTER_EIP    :
            (decoder->stack_width == ZYDIS_STACK_WIDTH_32) ? ZYDIS_REGISTER_EIP    :
                                                             ZYDIS_REGISTER_RIP;
        break;
    case ZYDIS_IMPLREG_TYPE_FLAGS_SSZ:
        operand->reg.value =
            (decoder->stack_width == ZYDIS_STACK_WIDTH_16) ? ZYDIS_REGISTER_FLAGS  :
            (decoder->stack_width == ZYDIS_STACK_WIDTH_32) ? ZYDIS_REGISTER_EFLAGS :
                                                             ZYDIS_REGISTER_RFLAGS;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Decodes an implicit memory operand.
 *
 * @param   decoder         A pointer to the `ZydisDecoder` instance.
 * @param   context         A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction     A pointer to the `ZydisDecodedInstruction` struct.
 * @param   operand         A pointer to the `ZydisDecodedOperand` struct.
 * @param   definition      A pointer to the `ZydisOperandDefinition` struct.
 */
static void ZydisDecodeOperandImplicitMemory(const ZydisDecoder* decoder,
    const ZydisDecoderContext* context, const ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand* operand, const ZydisOperandDefinition* definition)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(operand);
    ZYAN_ASSERT(definition);

    static const ZydisRegisterClass lookup[3] =
    {
        ZYDIS_REGCLASS_GPR16,
        ZYDIS_REGCLASS_GPR32,
        ZYDIS_REGCLASS_GPR64
    };

    operand->type = ZYDIS_OPERAND_TYPE_MEMORY;
    operand->mem.type = ZYDIS_MEMOP_TYPE_MEM;

    switch (definition->op.mem.base)
    {
    case ZYDIS_IMPLMEM_BASE_AGPR_REG:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index],
            ZydisCalcRegisterId(context, instruction, ZYDIS_REG_ENCODING_REG,
                lookup[context->easz_index]));
        break;
    case ZYDIS_IMPLMEM_BASE_AGPR_RM:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index],
            ZydisCalcRegisterId(context, instruction, ZYDIS_REG_ENCODING_RM,
                lookup[context->easz_index]));
        break;
    case ZYDIS_IMPLMEM_BASE_AAX:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index], 0);
        break;
    case ZYDIS_IMPLMEM_BASE_ADX:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index], 2);
        break;
    case ZYDIS_IMPLMEM_BASE_ABX:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index], 3);
        break;
    case ZYDIS_IMPLMEM_BASE_ASI:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index], 6);
        break;
    case ZYDIS_IMPLMEM_BASE_ADI:
        operand->mem.base = ZydisRegisterEncode(lookup[context->easz_index], 7);
        break;
    case ZYDIS_IMPLMEM_BASE_SSP:
        operand->mem.base = ZydisRegisterEncode(lookup[decoder->stack_width], 4);
        break;
    case ZYDIS_IMPLMEM_BASE_SBP:
        operand->mem.base = ZydisRegisterEncode(lookup[decoder->stack_width], 5);
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    if (definition->op.mem.seg)
    {
        operand->mem.segment =
            ZydisRegisterEncode(ZYDIS_REGCLASS_SEGMENT, definition->op.mem.seg - 1);
        ZYAN_ASSERT(operand->mem.segment);
    }
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
static ZyanStatus ZydisDecodeOperands(const ZydisDecoder* decoder, const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZydisDecodedOperand* operands, ZyanU8 operand_count)
{
    ZYAN_ASSERT(decoder);
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(context->definition);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(operands);
    ZYAN_ASSERT(operand_count);
    ZYAN_ASSERT(operand_count <= instruction->operand_count);

    const ZydisInstructionDefinition* definition = context->definition;
    const ZydisOperandDefinition* operand = ZydisGetOperandDefinitions(definition);

    ZYAN_MEMSET(operands, 0, sizeof(ZydisDecodedOperand) * operand_count);

    ZyanU8 imm_id = 0;
    for (ZyanU8 i = 0; i < operand_count; ++i)
    {
        ZydisRegisterClass register_class = ZYDIS_REGCLASS_INVALID;

        operands[i].id = i;
        operands[i].visibility = operand->visibility;
        operands[i].actions = operand->actions;
        ZYAN_ASSERT(!(operand->actions &
            ZYDIS_OPERAND_ACTION_READ & ZYDIS_OPERAND_ACTION_CONDREAD) ||
            (operand->actions & ZYDIS_OPERAND_ACTION_READ) ^
            (operand->actions & ZYDIS_OPERAND_ACTION_CONDREAD));
        ZYAN_ASSERT(!(operand->actions &
            ZYDIS_OPERAND_ACTION_WRITE & ZYDIS_OPERAND_ACTION_CONDWRITE) ||
            (operand->actions & ZYDIS_OPERAND_ACTION_WRITE) ^
            (operand->actions & ZYDIS_OPERAND_ACTION_CONDWRITE));

        // Implicit operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_REG:
            ZydisDecodeOperandImplicitRegister(decoder, context, instruction, &operands[i], operand);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_MEM:
            ZydisDecodeOperandImplicitMemory(decoder, context, instruction, &operands[i], operand);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_IMM1:
            operands[i].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            operands[i].size = 8;
            operands[i].imm.value.u = 1;
            operands[i].imm.is_signed = ZYAN_FALSE;
            operands[i].imm.is_relative = ZYAN_FALSE;
            break;
        default:
            break;
        }
        if (operands[i].type)
        {
            goto FinalizeOperand;
        }

        operands[i].encoding = operand->op.encoding;

        // Register operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_GPR8:
            register_class = ZYDIS_REGCLASS_GPR8;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR16:
            register_class = ZYDIS_REGCLASS_GPR16;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR32:
            register_class = ZYDIS_REGCLASS_GPR32;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR64:
            register_class = ZYDIS_REGCLASS_GPR64;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR16_32_64:
            ZYAN_ASSERT((instruction->operand_width == 16) || (instruction->operand_width == 32) ||
                (instruction->operand_width == 64));
            register_class =
                (instruction->operand_width == 16) ? ZYDIS_REGCLASS_GPR16 : (
                    (instruction->operand_width == 32) ? ZYDIS_REGCLASS_GPR32 : ZYDIS_REGCLASS_GPR64);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR32_32_64:
            ZYAN_ASSERT((instruction->operand_width == 16) || (instruction->operand_width == 32) ||
                (instruction->operand_width == 64));
            register_class =
                (instruction->operand_width == 16) ? ZYDIS_REGCLASS_GPR32 : (
                    (instruction->operand_width == 32) ? ZYDIS_REGCLASS_GPR32 : ZYDIS_REGCLASS_GPR64);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR16_32_32:
            ZYAN_ASSERT((instruction->operand_width == 16) || (instruction->operand_width == 32) ||
                (instruction->operand_width == 64));
            register_class =
                (instruction->operand_width == 16) ? ZYDIS_REGCLASS_GPR16 : ZYDIS_REGCLASS_GPR32;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_GPR_ASZ:
            ZYAN_ASSERT((instruction->address_width == 16) || (instruction->address_width == 32) ||
                (instruction->address_width == 64));
            register_class =
                (instruction->address_width == 16) ? ZYDIS_REGCLASS_GPR16 : (
                    (instruction->address_width == 32) ? ZYDIS_REGCLASS_GPR32 : ZYDIS_REGCLASS_GPR64);
            break;
        case ZYDIS_SEMANTIC_OPTYPE_FPR:
            register_class = ZYDIS_REGCLASS_X87;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MMX:
            register_class = ZYDIS_REGCLASS_MMX;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_XMM:
            register_class = ZYDIS_REGCLASS_XMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_YMM:
            register_class = ZYDIS_REGCLASS_YMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_ZMM:
            register_class = ZYDIS_REGCLASS_ZMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_TMM:
            register_class = ZYDIS_REGCLASS_TMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_BND:
            register_class = ZYDIS_REGCLASS_BOUND;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_SREG:
            register_class = ZYDIS_REGCLASS_SEGMENT;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_CR:
            register_class = ZYDIS_REGCLASS_CONTROL;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_DR:
            register_class = ZYDIS_REGCLASS_DEBUG;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MASK:
            register_class = ZYDIS_REGCLASS_MASK;
            break;
        default:
            break;
        }
        if (register_class)
        {
            switch (operand->op.encoding)
            {
            case ZYDIS_OPERAND_ENCODING_MODRM_REG:
                ZYAN_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &operands[i], register_class,
                        ZydisCalcRegisterId(
                            context, instruction, ZYDIS_REG_ENCODING_REG, register_class)));
                break;
            case ZYDIS_OPERAND_ENCODING_MODRM_RM:
                ZYAN_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &operands[i], register_class,
                        ZydisCalcRegisterId(
                            context, instruction, ZYDIS_REG_ENCODING_RM, register_class)));
                break;
            case ZYDIS_OPERAND_ENCODING_OPCODE:
                ZYAN_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &operands[i], register_class,
                        ZydisCalcRegisterId(
                            context, instruction, ZYDIS_REG_ENCODING_OPCODE, register_class)));
                break;
            case ZYDIS_OPERAND_ENCODING_NDSNDD:
                ZYAN_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &operands[i], register_class,
                        ZydisCalcRegisterId(
                            context, instruction, ZYDIS_REG_ENCODING_NDSNDD, register_class)));
                break;
            case ZYDIS_OPERAND_ENCODING_MASK:
                ZYAN_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &operands[i], register_class,
                        ZydisCalcRegisterId(
                            context, instruction, ZYDIS_REG_ENCODING_MASK, register_class)));
                break;
            case ZYDIS_OPERAND_ENCODING_IS4:
                ZYAN_CHECK(
                    ZydisDecodeOperandRegister(
                        instruction, &operands[i], register_class,
                        ZydisCalcRegisterId(
                            context, instruction, ZYDIS_REG_ENCODING_IS4, register_class)));
                break;
            default:
                ZYAN_UNREACHABLE;
            }

            if (operand->is_multisource4)
            {
                operands[i].attributes |= ZYDIS_OATTRIB_IS_MULTISOURCE4;
            }

            goto FinalizeOperand;
        }

        // Memory operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_MEM:
            ZYAN_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &operands[i], ZYDIS_REGCLASS_INVALID));
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBX:
            ZYAN_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &operands[i], ZYDIS_REGCLASS_XMM));
            operands[i].mem.type = ZYDIS_MEMOP_TYPE_VSIB;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBY:
            ZYAN_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &operands[i], ZYDIS_REGCLASS_YMM));
            operands[i].mem.type = ZYDIS_MEMOP_TYPE_VSIB;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBZ:
            ZYAN_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &operands[i], ZYDIS_REGCLASS_ZMM));
            operands[i].mem.type = ZYDIS_MEMOP_TYPE_VSIB;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_PTR:
            ZYAN_ASSERT((instruction->raw.imm[0].size == 16) ||
                (instruction->raw.imm[0].size == 32));
            ZYAN_ASSERT(instruction->raw.imm[1].size == 16);
            operands[i].type = ZYDIS_OPERAND_TYPE_POINTER;
            operands[i].ptr.offset = (ZyanU32)instruction->raw.imm[0].value.u;
            operands[i].ptr.segment = (ZyanU16)instruction->raw.imm[1].value.u;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_AGEN:
            operands[i].actions = 0; // TODO: Remove after generator update
            ZYAN_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &operands[i], ZYDIS_REGCLASS_INVALID));
            operands[i].mem.type = ZYDIS_MEMOP_TYPE_AGEN;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MOFFS:
            ZYAN_ASSERT(instruction->raw.disp.size);
            operands[i].type = ZYDIS_OPERAND_TYPE_MEMORY;
            operands[i].mem.type = ZYDIS_MEMOP_TYPE_MEM;
            operands[i].mem.disp.has_displacement = ZYAN_TRUE;
            operands[i].mem.disp.value = instruction->raw.disp.value;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MIB:
            operands[i].actions = 0; // TODO: Remove after generator update
            ZYAN_CHECK(
                ZydisDecodeOperandMemory(
                    context, instruction, &operands[i], ZYDIS_REGCLASS_INVALID));
            operands[i].mem.type = ZYDIS_MEMOP_TYPE_MIB;
            break;
        default:
            break;
        }
        if (operands[i].type)
        {
#if !defined(ZYDIS_DISABLE_AVX512) || !defined(ZYDIS_DISABLE_KNC)
            // Handle compressed 8-bit displacement
            if (((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX)) &&
                (instruction->raw.disp.size == 8))
            {
                operands[i].mem.disp.value *= context->cd8_scale;
            }
#endif

            goto FinalizeOperand;
        }

        // Immediate operands
        switch (operand->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_REL:
            ZYAN_ASSERT(instruction->raw.imm[imm_id].is_relative);
            ZYAN_FALLTHROUGH;
        case ZYDIS_SEMANTIC_OPTYPE_IMM:
            ZYAN_ASSERT((imm_id == 0) || (imm_id == 1));
            operands[i].type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
            operands[i].size = operand->size[context->eosz_index] * 8;
            if (operand->op.encoding == ZYDIS_OPERAND_ENCODING_IS4)
            {
                // The upper half of the 8-bit immediate is used to encode a register specifier
                ZYAN_ASSERT(instruction->raw.imm[imm_id].size == 8);
                operands[i].imm.value.u = (ZyanU8)instruction->raw.imm[imm_id].value.u & 0x0F;
            }
            else
            {
                operands[i].imm.value.u = instruction->raw.imm[imm_id].value.u;
            }
            operands[i].imm.is_signed = instruction->raw.imm[imm_id].is_signed;
            operands[i].imm.is_relative = instruction->raw.imm[imm_id].is_relative;
            ++imm_id;
            break;
        default:
            break;
        }
        ZYAN_ASSERT(operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE);

    FinalizeOperand:
        // Set segment-register for memory operands
        if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
        {
            if (!operand->ignore_seg_override &&
                instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_CS)
            {
                operands[i].mem.segment = ZYDIS_REGISTER_CS;
            }
            else
                if (!operand->ignore_seg_override &&
                    instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_SS)
                {
                    operands[i].mem.segment = ZYDIS_REGISTER_SS;
                }
                else
                    if (!operand->ignore_seg_override &&
                        instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_DS)
                    {
                        operands[i].mem.segment = ZYDIS_REGISTER_DS;
                    }
                    else
                        if (!operand->ignore_seg_override &&
                            instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_ES)
                        {
                            operands[i].mem.segment = ZYDIS_REGISTER_ES;
                        }
                        else
                            if (!operand->ignore_seg_override &&
                                instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS)
                            {
                                operands[i].mem.segment = ZYDIS_REGISTER_FS;
                            }
                            else
                                if (!operand->ignore_seg_override &&
                                    instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_GS)
                                {
                                    operands[i].mem.segment = ZYDIS_REGISTER_GS;
                                }
                                else
                                {
                                    if (operands[i].mem.segment == ZYDIS_REGISTER_NONE)
                                    {
                                        if ((operands[i].mem.base == ZYDIS_REGISTER_RSP) ||
                                            (operands[i].mem.base == ZYDIS_REGISTER_RBP) ||
                                            (operands[i].mem.base == ZYDIS_REGISTER_ESP) ||
                                            (operands[i].mem.base == ZYDIS_REGISTER_EBP) ||
                                            (operands[i].mem.base == ZYDIS_REGISTER_SP) ||
                                            (operands[i].mem.base == ZYDIS_REGISTER_BP))
                                        {
                                            operands[i].mem.segment = ZYDIS_REGISTER_SS;
                                        }
                                        else
                                        {
                                            operands[i].mem.segment = ZYDIS_REGISTER_DS;
                                        }
                                    }
                                }
        }

        ZydisSetOperandSizeAndElementInfo(context, instruction, &operands[i], operand);
        ++operand;
    }

#if !defined(ZYDIS_DISABLE_AVX512) || !defined(ZYDIS_DISABLE_KNC)
    // Fix operand-action for EVEX/MVEX instructions with merge-mask
    if (instruction->avx.mask.mode == ZYDIS_MASK_MODE_MERGING)
    {
        ZYAN_ASSERT(operand_count >= 1);
        switch (operands[0].actions)
        {
        case ZYDIS_OPERAND_ACTION_WRITE:
            if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY)
            {
                operands[0].actions = ZYDIS_OPERAND_ACTION_CONDWRITE;
            }
            else
            {
                operands[0].actions = ZYDIS_OPERAND_ACTION_READ_CONDWRITE;
            }
            break;
        case ZYDIS_OPERAND_ACTION_READWRITE:
            operands[0].actions = ZYDIS_OPERAND_ACTION_READ_CONDWRITE;
            break;
        default:
            break;
        }
    }
#endif

    return ZYAN_STATUS_SUCCESS;
}
#endif

/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Sets attributes for the given instruction.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   definition  A pointer to the `ZydisInstructionDefinition` struct.
 */
static void ZydisSetAttributes(ZydisDecoderState* state, ZydisDecodedInstruction* instruction,
    const ZydisInstructionDefinition* definition)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(definition);

    if (definition->cpu_state != ZYDIS_RW_ACTION_NONE)
    {
        static const ZydisInstructionAttributes mapping[ZYDIS_RW_ACTION_MAX_VALUE + 1] =
        {
            /* NONE      */ 0,
            /* READ      */ ZYDIS_ATTRIB_CPU_STATE_CR,
            /* WRITE     */ ZYDIS_ATTRIB_CPU_STATE_CW,
            /* READWRITE */ ZYDIS_ATTRIB_CPU_STATE_CR | ZYDIS_ATTRIB_CPU_STATE_CW
        };
        ZYAN_ASSERT(definition->cpu_state < ZYAN_ARRAY_LENGTH(mapping));
        instruction->attributes |= mapping[definition->cpu_state];
    }

    if (definition->fpu_state != ZYDIS_RW_ACTION_NONE)
    {
        static const ZydisInstructionAttributes mapping[ZYDIS_RW_ACTION_MAX_VALUE + 1] =
        {
            /* NONE      */ 0,
            /* READ      */ ZYDIS_ATTRIB_FPU_STATE_CR,
            /* WRITE     */ ZYDIS_ATTRIB_FPU_STATE_CW,
            /* READWRITE */ ZYDIS_ATTRIB_FPU_STATE_CR | ZYDIS_ATTRIB_FPU_STATE_CW
        };
        ZYAN_ASSERT(definition->fpu_state < ZYAN_ARRAY_LENGTH(mapping));
        instruction->attributes |= mapping[definition->fpu_state];
    }

    if (definition->xmm_state != ZYDIS_RW_ACTION_NONE)
    {
        static const ZydisInstructionAttributes mapping[ZYDIS_RW_ACTION_MAX_VALUE + 1] =
        {
            /* NONE      */ 0,
            /* READ      */ ZYDIS_ATTRIB_XMM_STATE_CR,
            /* WRITE     */ ZYDIS_ATTRIB_XMM_STATE_CW,
            /* READWRITE */ ZYDIS_ATTRIB_XMM_STATE_CR | ZYDIS_ATTRIB_XMM_STATE_CW
        };
        ZYAN_ASSERT(definition->xmm_state < ZYAN_ARRAY_LENGTH(mapping));
        instruction->attributes |= mapping[definition->xmm_state];
    }

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
    {
        const ZydisInstructionDefinitionLEGACY* def =
            (const ZydisInstructionDefinitionLEGACY*)definition;

        if (def->is_privileged)
        {
            instruction->attributes |= ZYDIS_ATTRIB_IS_PRIVILEGED;
        }
        if (def->accepts_LOCK)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_LOCK;
            if (state->prefixes.has_lock)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_LOCK;
                instruction->raw.prefixes[state->prefixes.offset_lock].type =
                    ZYDIS_PREFIX_TYPE_EFFECTIVE;
            }
        }
        if (def->accepts_REP)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_REP;
        }
        if (def->accepts_REPEREPZ)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_REPE;
        }
        if (def->accepts_REPNEREPNZ)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_REPNE;
        }
        if (def->accepts_BOUND)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_BND;
        }
        if (def->accepts_XACQUIRE)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_XACQUIRE;
        }
        if (def->accepts_XRELEASE)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_XRELEASE;
        }
        if (def->accepts_hle_without_lock)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_HLE_WITHOUT_LOCK;
        }

        switch (state->prefixes.group1)
        {
        case 0xF2:
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_REPNE)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_REPNE;
                break;
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_XACQUIRE)
            {
                if ((instruction->attributes & ZYDIS_ATTRIB_HAS_LOCK) ||
                    (def->accepts_hle_without_lock))
                {
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_XACQUIRE;
                    break;
                }
            }
            if ((state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_MPX)) &&
                instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_BND)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_BND;
                break;
            }
            break;
        case 0xF3:
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_REP)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_REP;
                break;
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_REPE)
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_REPE;
                break;
            }
            if (instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_XRELEASE)
            {
                if ((instruction->attributes & ZYDIS_ATTRIB_HAS_LOCK) ||
                    (def->accepts_hle_without_lock))
                {
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_XRELEASE;
                    break;
                }
            }
            break;
        default:
            break;
        }
        if ((instruction->raw.prefixes[state->prefixes.offset_group1].type ==
             ZYDIS_PREFIX_TYPE_IGNORED) &&
            (instruction->attributes & (
             ZYDIS_ATTRIB_HAS_REP | ZYDIS_ATTRIB_HAS_REPE | ZYDIS_ATTRIB_HAS_REPNE |
             ZYDIS_ATTRIB_HAS_BND | ZYDIS_ATTRIB_HAS_XACQUIRE | ZYDIS_ATTRIB_HAS_XRELEASE)))
        {
            instruction->raw.prefixes[state->prefixes.offset_group1].type =
                ZYDIS_PREFIX_TYPE_EFFECTIVE;
        }

        if (def->accepts_branch_hints)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_BRANCH_HINTS;
            switch (state->prefixes.group2)
            {
            case 0x2E:
                instruction->attributes |= ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN;
                instruction->raw.prefixes[state->prefixes.offset_group2].type =
                    ZYDIS_PREFIX_TYPE_EFFECTIVE;
                break;
            case 0x3E:
                instruction->attributes |= ZYDIS_ATTRIB_HAS_BRANCH_TAKEN;
                instruction->raw.prefixes[state->prefixes.offset_group2].type =
                    ZYDIS_PREFIX_TYPE_EFFECTIVE;
                break;
            default:
                break;
            }
        }

        if (def->accepts_NOTRACK)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_NOTRACK;
            if ((state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_CET)) &&
                (state->prefixes.offset_notrack >= 0))
            {
                instruction->attributes |= ZYDIS_ATTRIB_HAS_NOTRACK;
                instruction->raw.prefixes[state->prefixes.offset_notrack].type =
                    ZYDIS_PREFIX_TYPE_EFFECTIVE;
            }
        }

        if (def->accepts_segment && !def->accepts_branch_hints)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_SEGMENT;
            if (state->prefixes.effective_segment &&
                !(instruction->attributes & ZYDIS_ATTRIB_HAS_NOTRACK))
            {
                switch (state->prefixes.effective_segment)
                {
                case 0x2E:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_CS;
                    break;
                case 0x36:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_SS;
                    break;
                case 0x3E:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_DS;
                    break;
                case 0x26:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_ES;
                    break;
                case 0x64:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_FS;
                    break;
                case 0x65:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_GS;
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
            }
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT)
            {
                instruction->raw.prefixes[state->prefixes.offset_segment].type =
                    ZYDIS_PREFIX_TYPE_EFFECTIVE;
            }
        }

        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        if (definition->accepts_segment)
        {
            instruction->attributes |= ZYDIS_ATTRIB_ACCEPTS_SEGMENT;
            if (state->prefixes.effective_segment)
            {
                switch (state->prefixes.effective_segment)
                {
                case 0x2E:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_CS;
                    break;
                case 0x36:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_SS;
                    break;
                case 0x3E:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_DS;
                    break;
                case 0x26:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_ES;
                    break;
                case 0x64:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_FS;
                    break;
                case 0x65:
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_SEGMENT_GS;
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
            }
            if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT)
            {
                instruction->raw.prefixes[state->prefixes.offset_segment].type =
                    ZYDIS_PREFIX_TYPE_EFFECTIVE;
            }
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }
}
#endif

#ifndef ZYDIS_MINIMAL_MODE
/**
 * Sets AVX-specific information for the given instruction.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   definition  A pointer to the `ZydisInstructionDefinition` struct.
 *
 * Information set for `XOP`:
 * - Vector Length
 *
 * Information set for `VEX`:
 * - Vector length
 * - Static broadcast-factor
 *
 * Information set for `EVEX`:
 * - Vector length
 * - Broadcast-factor (static and dynamic)
 * - Rounding-mode and SAE
 * - Mask mode
 * - Compressed 8-bit displacement scale-factor
 *
 * Information set for `MVEX`:
 * - Vector length
 * - Broadcast-factor (static and dynamic)
 * - Rounding-mode and SAE
 * - Swizzle- and conversion-mode
 * - Mask mode
 * - Eviction hint
 * - Compressed 8-bit displacement scale-factor
 */
static void ZydisSetAVXInformation(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(definition);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    {
        // Vector length
        static const ZyanU16 lookup[2] =
        {
            128,
            256
        };
        ZYAN_ASSERT(context->vector_unified.LL < ZYAN_ARRAY_LENGTH(lookup));
        instruction->avx.vector_length = lookup[context->vector_unified.LL];
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    {
        // Vector length
        static const ZyanU16 lookup[2] =
        {
            128,
            256
        };
        ZYAN_ASSERT(context->vector_unified.LL < ZYAN_ARRAY_LENGTH(lookup));
        instruction->avx.vector_length = lookup[context->vector_unified.LL];

        // Static broadcast-factor
        const ZydisInstructionDefinitionVEX* def =
            (const ZydisInstructionDefinitionVEX*)definition;
        if (def->broadcast)
        {
            instruction->avx.broadcast.is_static = ZYAN_TRUE;
            static ZydisBroadcastMode broadcasts[ZYDIS_VEX_STATIC_BROADCAST_MAX_VALUE + 1] =
            {
                ZYDIS_BROADCAST_MODE_INVALID,
                ZYDIS_BROADCAST_MODE_1_TO_2,
                ZYDIS_BROADCAST_MODE_1_TO_4,
                ZYDIS_BROADCAST_MODE_1_TO_8,
                ZYDIS_BROADCAST_MODE_1_TO_16,
                ZYDIS_BROADCAST_MODE_1_TO_32,
                ZYDIS_BROADCAST_MODE_2_TO_4
            };
            instruction->avx.broadcast.mode = broadcasts[def->broadcast];
        }
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    {
#ifndef ZYDIS_DISABLE_AVX512
        const ZydisInstructionDefinitionEVEX* def =
            (const ZydisInstructionDefinitionEVEX*)definition;

        // Vector length
        ZyanU8 vector_length = context->vector_unified.LL;
        if (def->vector_length)
        {
            vector_length = def->vector_length - 1;
        }
        static const ZyanU16 lookup[3] =
        {
            128,
            256,
            512
        };
        ZYAN_ASSERT(vector_length < ZYAN_ARRAY_LENGTH(lookup));
        instruction->avx.vector_length = lookup[vector_length];

        context->evex.tuple_type = def->tuple_type;
        if (def->tuple_type)
        {
            ZYAN_ASSERT(instruction->raw.modrm.mod != 3);
            ZYAN_ASSERT(def->element_size);

            // Element size
            static const ZyanU8 element_sizes[ZYDIS_IELEMENT_SIZE_MAX_VALUE + 1] =
            {
                  0,   8,  16,  32,  64, 128
            };
            ZYAN_ASSERT(def->element_size < ZYAN_ARRAY_LENGTH(element_sizes));
            context->evex.element_size = element_sizes[def->element_size];

            // Compressed disp8 scale and broadcast-factor
            switch (def->tuple_type)
            {
            case ZYDIS_TUPLETYPE_FV:
            {
                const ZyanU8 evex_b = instruction->raw.evex.b;
                ZYAN_ASSERT(evex_b < 2);
                ZYAN_ASSERT(!evex_b || ((!context->vector_unified.W && (context->evex.element_size == 16 ||
                                                                        context->evex.element_size == 32)) ||
                                        ( context->vector_unified.W &&  context->evex.element_size == 64)));
                ZYAN_ASSERT(!evex_b || def->functionality == ZYDIS_EVEX_FUNC_BC);

                static const ZyanU8 scales[2][3][3] =
                {
                    /*B0*/ { /*16*/ { 16, 32, 64 }, /*32*/ { 16, 32, 64 }, /*64*/ { 16, 32, 64 } },
                    /*B1*/ { /*16*/ {  2,  2,  2 }, /*32*/ {  4,  4,  4 }, /*64*/ {  8,  8,  8 } }
                };
                static const ZydisBroadcastMode broadcasts[2][3][3] =
                {
                    /*B0*/
                    {
                        /*16*/
                        {
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID
                        },
                        /*32*/
                        {
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID
                        },
                        /*64*/
                        {
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID
                        }
                    },
                    /*B1*/
                    {
                        /*16*/
                        {
                            ZYDIS_BROADCAST_MODE_1_TO_8,
                            ZYDIS_BROADCAST_MODE_1_TO_16,
                            ZYDIS_BROADCAST_MODE_1_TO_32
                        },
                        /*32*/
                        {
                            ZYDIS_BROADCAST_MODE_1_TO_4,
                            ZYDIS_BROADCAST_MODE_1_TO_8,
                            ZYDIS_BROADCAST_MODE_1_TO_16
                        },
                        /*64*/
                        {
                            ZYDIS_BROADCAST_MODE_1_TO_2,
                            ZYDIS_BROADCAST_MODE_1_TO_4,
                            ZYDIS_BROADCAST_MODE_1_TO_8
                        }
                    }
                };

                const ZyanU8 size_index = context->evex.element_size >> 5;
                ZYAN_ASSERT(size_index < 3);

                context->cd8_scale = scales[evex_b][size_index][vector_length];
                instruction->avx.broadcast.mode = broadcasts[evex_b][size_index][vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_HV:
            {
                const ZyanU8 evex_b = instruction->raw.evex.b;
                ZYAN_ASSERT(evex_b < 2);
                ZYAN_ASSERT(!context->vector_unified.W);
                ZYAN_ASSERT((context->evex.element_size == 16) ||
                            (context->evex.element_size == 32));
                ZYAN_ASSERT(!evex_b || def->functionality == ZYDIS_EVEX_FUNC_BC);

                static const ZyanU8 scales[2][2][3] =
                {
                    /*B0*/ { /*16*/ {  8, 16, 32 }, /*32*/ {  8, 16, 32 } },
                    /*B1*/ { /*16*/ {  2,  2,  2 }, /*32*/ {  4,  4,  4 } }
                };
                static const ZydisBroadcastMode broadcasts[2][2][3] =
                {
                    /*B0*/
                    {
                        /*16*/
                        {
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID
                        },
                        /*32*/
                        {
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID,
                            ZYDIS_BROADCAST_MODE_INVALID
                        }
                    },
                    /*B1*/
                    {
                        /*16*/
                        {
                            ZYDIS_BROADCAST_MODE_1_TO_4,
                            ZYDIS_BROADCAST_MODE_1_TO_8,
                            ZYDIS_BROADCAST_MODE_1_TO_16
                        },
                        /*32*/
                        {
                            ZYDIS_BROADCAST_MODE_1_TO_2,
                            ZYDIS_BROADCAST_MODE_1_TO_4,
                            ZYDIS_BROADCAST_MODE_1_TO_8
                        }
                    }
                };

                const ZyanU8 size_index = context->evex.element_size >> 5;
                ZYAN_ASSERT(size_index < 3);

                context->cd8_scale = scales[evex_b][size_index][vector_length];
                instruction->avx.broadcast.mode = broadcasts[evex_b][size_index][vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_FVM:
            {
                static const ZyanU8 scales[3] =
                {
                    16, 32, 64
                };
                context->cd8_scale = scales[vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_GSCAT:
                switch (context->vector_unified.W)
                {
                case 0:
                    ZYAN_ASSERT(context->evex.element_size == 32);
                    break;
                case 1:
                    ZYAN_ASSERT(context->evex.element_size == 64);
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
                ZYAN_FALLTHROUGH;
            case ZYDIS_TUPLETYPE_T1S:
            {
                static const ZyanU8 scales[6] =
                {
                    /*   */  0,
                    /*  8*/  1,
                    /* 16*/  2,
                    /* 32*/  4,
                    /* 64*/  8,
                    /*128*/ 16,
                };
                ZYAN_ASSERT(def->element_size < ZYAN_ARRAY_LENGTH(scales));
                context->cd8_scale = scales[def->element_size];
                break;
            };
            case ZYDIS_TUPLETYPE_T1F:
            {
                static const ZyanU8 scales[3] =
                {
                    /* 16*/ 2,
                    /* 32*/ 4,
                    /* 64*/ 8
                };

                const ZyanU8 size_index = context->evex.element_size >> 5;
                ZYAN_ASSERT(size_index < 3);

                context->cd8_scale = scales[size_index];
                break;
            }
            case ZYDIS_TUPLETYPE_T1_4X:
                ZYAN_ASSERT(context->evex.element_size == 32);
                ZYAN_ASSERT(context->vector_unified.W == 0);
                context->cd8_scale = 16;
                break;
            case ZYDIS_TUPLETYPE_T2:
                switch (context->vector_unified.W)
                {
                case 0:
                    ZYAN_ASSERT(context->evex.element_size == 32);
                    context->cd8_scale = 8;
                    break;
                case 1:
                    ZYAN_ASSERT(context->evex.element_size == 64);
                    ZYAN_ASSERT((instruction->avx.vector_length == 256) ||
                                (instruction->avx.vector_length == 512));
                    context->cd8_scale = 16;
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_T4:
                switch (context->vector_unified.W)
                {
                case 0:
                    ZYAN_ASSERT(context->evex.element_size == 32);
                    ZYAN_ASSERT((instruction->avx.vector_length == 256) ||
                                (instruction->avx.vector_length == 512));
                    context->cd8_scale = 16;
                    break;
                case 1:
                    ZYAN_ASSERT(context->evex.element_size == 64);
                    ZYAN_ASSERT(instruction->avx.vector_length == 512);
                    context->cd8_scale = 32;
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
                break;
            case ZYDIS_TUPLETYPE_T8:
                ZYAN_ASSERT(!context->vector_unified.W);
                ZYAN_ASSERT(instruction->avx.vector_length == 512);
                ZYAN_ASSERT(context->evex.element_size == 32);
                context->cd8_scale = 32;
                break;
            case ZYDIS_TUPLETYPE_HVM:
            {
                static const ZyanU8 scales[3] =
                {
                     8, 16, 32
                };
                context->cd8_scale = scales[vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_QVM:
            {
                static const ZyanU8 scales[3] =
                {
                     4,  8, 16
                };
                context->cd8_scale = scales[vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_OVM:
            {
                static const ZyanU8 scales[3] =
                {
                     2,  4,  8
                };
                context->cd8_scale = scales[vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_M128:
                context->cd8_scale = 16;
                break;
            case ZYDIS_TUPLETYPE_DUP:
            {
                static const ZyanU8 scales[3] =
                {
                     8, 32, 64
                };
                context->cd8_scale = scales[vector_length];
                break;
            }
            case ZYDIS_TUPLETYPE_QUARTER:
            {
                const ZyanU8 evex_b = instruction->raw.evex.b;
                ZYAN_ASSERT(evex_b < 2);
                ZYAN_ASSERT(!context->vector_unified.W);
                ZYAN_ASSERT(context->evex.element_size == 16);
                ZYAN_ASSERT(!evex_b || def->functionality == ZYDIS_EVEX_FUNC_BC);

                static const ZyanU8 scales[2][3] =
                {
                    /*B0*/ {  4,  8, 16 },
                    /*B1*/ {  2,  2,  2 }
                };
                static const ZydisBroadcastMode broadcasts[2][3] =
                {
                    /*B0*/
                    {
                        ZYDIS_BROADCAST_MODE_INVALID,
                        ZYDIS_BROADCAST_MODE_INVALID,
                        ZYDIS_BROADCAST_MODE_INVALID
                    },
                    /*B1*/
                    {
                        ZYDIS_BROADCAST_MODE_1_TO_2,
                        ZYDIS_BROADCAST_MODE_1_TO_4,
                        ZYDIS_BROADCAST_MODE_1_TO_8
                    }
                };
                context->cd8_scale = scales[evex_b][vector_length];
                instruction->avx.broadcast.mode = broadcasts[evex_b][vector_length];
                break;
            }
            default:
                ZYAN_UNREACHABLE;
            }
        } else
        {
            ZYAN_ASSERT(instruction->raw.modrm.mod == 3);
        }

        // Static broadcast-factor
        if (def->broadcast)
        {
            ZYAN_ASSERT(!instruction->avx.broadcast.mode);
            instruction->avx.broadcast.is_static = ZYAN_TRUE;
            static const ZydisBroadcastMode broadcasts[ZYDIS_EVEX_STATIC_BROADCAST_MAX_VALUE + 1] =
            {
                ZYDIS_BROADCAST_MODE_INVALID,
                ZYDIS_BROADCAST_MODE_1_TO_2,
                ZYDIS_BROADCAST_MODE_1_TO_4,
                ZYDIS_BROADCAST_MODE_1_TO_8,
                ZYDIS_BROADCAST_MODE_1_TO_16,
                ZYDIS_BROADCAST_MODE_1_TO_32,
                ZYDIS_BROADCAST_MODE_1_TO_64,
                ZYDIS_BROADCAST_MODE_2_TO_4,
                ZYDIS_BROADCAST_MODE_2_TO_8,
                ZYDIS_BROADCAST_MODE_2_TO_16,
                ZYDIS_BROADCAST_MODE_4_TO_8,
                ZYDIS_BROADCAST_MODE_4_TO_16,
                ZYDIS_BROADCAST_MODE_8_TO_16
            };
            ZYAN_ASSERT(def->broadcast < ZYAN_ARRAY_LENGTH(broadcasts));
            instruction->avx.broadcast.mode = broadcasts[def->broadcast];
        }

        // Rounding mode and SAE
        if (instruction->raw.evex.b)
        {
            switch (def->functionality)
            {
            case ZYDIS_EVEX_FUNC_INVALID:
            case ZYDIS_EVEX_FUNC_BC:
                // Noting to do here
                break;
            case ZYDIS_EVEX_FUNC_RC:
                instruction->avx.rounding.mode = ZYDIS_ROUNDING_MODE_RN + context->vector_unified.LL;
                ZYAN_FALLTHROUGH;
            case ZYDIS_EVEX_FUNC_SAE:
                instruction->avx.has_sae = ZYAN_TRUE;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
        }

        // Mask
        instruction->avx.mask.reg = ZYDIS_REGISTER_K0 + instruction->raw.evex.aaa;
        switch (def->mask_override)
        {
        case ZYDIS_MASK_OVERRIDE_DEFAULT:
            instruction->avx.mask.mode = ZYDIS_MASK_MODE_MERGING + instruction->raw.evex.z;
            break;
        case ZYDIS_MASK_OVERRIDE_ZEROING:
            instruction->avx.mask.mode = ZYDIS_MASK_MODE_ZEROING;
            break;
        case ZYDIS_MASK_OVERRIDE_CONTROL:
            instruction->avx.mask.mode = ZYDIS_MASK_MODE_CONTROL + instruction->raw.evex.z;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        if (!instruction->raw.evex.aaa)
        {
            instruction->avx.mask.mode = ZYDIS_MASK_MODE_DISABLED;
        }
#else
        ZYAN_UNREACHABLE;
#endif
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
    {
#ifndef ZYDIS_DISABLE_KNC
        // Vector length
        instruction->avx.vector_length = 512;

        const ZydisInstructionDefinitionMVEX* def =
            (const ZydisInstructionDefinitionMVEX*)definition;

        // Static broadcast-factor
        ZyanU8 index = def->has_element_granularity;
        ZYAN_ASSERT(!index || !def->broadcast);
        if (!index && def->broadcast)
        {
            instruction->avx.broadcast.is_static = ZYAN_TRUE;
            switch (def->broadcast)
            {
            case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                index = 1;
                break;
            case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                index = 1;
                break;
            case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_8:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_8;
                index = 2;
                break;
            case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_16:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                index = 2;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
        }

        // Compressed disp8 scale and broadcast-factor
        switch (def->functionality)
        {
        case ZYDIS_MVEX_FUNC_IGNORED:
        case ZYDIS_MVEX_FUNC_INVALID:
        case ZYDIS_MVEX_FUNC_RC:
        case ZYDIS_MVEX_FUNC_SAE:
        case ZYDIS_MVEX_FUNC_SWIZZLE_32:
        case ZYDIS_MVEX_FUNC_SWIZZLE_64:
            // Nothing to do here
            break;
        case ZYDIS_MVEX_FUNC_F_32:
        case ZYDIS_MVEX_FUNC_I_32:
        case ZYDIS_MVEX_FUNC_F_64:
        case ZYDIS_MVEX_FUNC_I_64:
            context->cd8_scale = 64;
            break;
        case ZYDIS_MVEX_FUNC_SF_32:
        case ZYDIS_MVEX_FUNC_SF_32_BCST:
        case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
        case ZYDIS_MVEX_FUNC_UF_32:
        {
            static const ZyanU8 lookup[3][8] =
            {
                { 64,  4, 16, 32, 16, 16, 32, 32 },
                {  4,  0,  0,  2,  1,  1,  2,  2 },
                { 16,  0,  0,  8,  4,  4,  8,  8 }
            };
            ZYAN_ASSERT(instruction->raw.mvex.SSS < ZYAN_ARRAY_LENGTH(lookup[index]));
            context->cd8_scale = lookup[index][instruction->raw.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_SI_32:
        case ZYDIS_MVEX_FUNC_UI_32:
        case ZYDIS_MVEX_FUNC_SI_32_BCST:
        case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
        {
            static const ZyanU8 lookup[3][8] =
            {
                { 64,  4, 16,  0, 16, 16, 32, 32 },
                {  4,  0,  0,  0,  1,  1,  2,  2 },
                { 16,  0,  0,  0,  4,  4,  8,  8 }
            };
            ZYAN_ASSERT(instruction->raw.mvex.SSS < ZYAN_ARRAY_LENGTH(lookup[index]));
            context->cd8_scale = lookup[index][instruction->raw.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_SF_64:
        case ZYDIS_MVEX_FUNC_UF_64:
        case ZYDIS_MVEX_FUNC_SI_64:
        case ZYDIS_MVEX_FUNC_UI_64:
        {
            static const ZyanU8 lookup[3][3] =
            {
                { 64,  8, 32 },
                {  8,  0,  0 },
                { 32,  0,  0 }
            };
            ZYAN_ASSERT(instruction->raw.mvex.SSS < ZYAN_ARRAY_LENGTH(lookup[index]));
            context->cd8_scale = lookup[index][instruction->raw.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_DF_32:
        case ZYDIS_MVEX_FUNC_DI_32:
        {
            static const ZyanU8 lookup[2][8] =
            {
                { 64,  0,  0, 32, 16, 16, 32, 32 },
                {  4,  0,  0,  2,  1,  1,  2,  2 }
            };
            ZYAN_ASSERT(index < 2);
            ZYAN_ASSERT(instruction->raw.mvex.SSS < ZYAN_ARRAY_LENGTH(lookup[index]));
            context->cd8_scale = lookup[index][instruction->raw.mvex.SSS];
            break;
        }
        case ZYDIS_MVEX_FUNC_DF_64:
        case ZYDIS_MVEX_FUNC_DI_64:
        {
            static const ZyanU8 lookup[2][1] =
            {
                { 64 },
                {  8 }
            };
            ZYAN_ASSERT(index < 2);
            ZYAN_ASSERT(instruction->raw.mvex.SSS < ZYAN_ARRAY_LENGTH(lookup[index]));
            context->cd8_scale = lookup[index][instruction->raw.mvex.SSS];
            break;
        }
        default:
            ZYAN_UNREACHABLE;
        }

        // Rounding mode, sae, swizzle, convert
        context->mvex.functionality = def->functionality;
        switch (def->functionality)
        {
        case ZYDIS_MVEX_FUNC_IGNORED:
        case ZYDIS_MVEX_FUNC_INVALID:
        case ZYDIS_MVEX_FUNC_F_32:
        case ZYDIS_MVEX_FUNC_I_32:
        case ZYDIS_MVEX_FUNC_F_64:
        case ZYDIS_MVEX_FUNC_I_64:
            // Nothing to do here
            break;
        case ZYDIS_MVEX_FUNC_RC:
            instruction->avx.rounding.mode = ZYDIS_ROUNDING_MODE_RN + (instruction->raw.mvex.SSS & 3);
            ZYAN_FALLTHROUGH;
        case ZYDIS_MVEX_FUNC_SAE:
            if (instruction->raw.mvex.SSS >= 4)
            {
                instruction->avx.has_sae = ZYAN_TRUE;
            }
            break;
        case ZYDIS_MVEX_FUNC_SWIZZLE_32:
        case ZYDIS_MVEX_FUNC_SWIZZLE_64:
            instruction->avx.swizzle.mode = ZYDIS_SWIZZLE_MODE_DCBA + instruction->raw.mvex.SSS;
            break;
        case ZYDIS_MVEX_FUNC_SF_32:
        case ZYDIS_MVEX_FUNC_SF_32_BCST:
        case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
            switch (instruction->raw.mvex.SSS)
            {
            case 0:
                break;
            case 1:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                break;
            case 2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                break;
            case 3:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_FLOAT16;
                break;
            case 4:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_SI_32:
        case ZYDIS_MVEX_FUNC_SI_32_BCST:
        case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
            switch (instruction->raw.mvex.SSS)
            {
            case 0:
                break;
            case 1:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_16;
                break;
            case 2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_16;
                break;
            case 4:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_SF_64:
        case ZYDIS_MVEX_FUNC_SI_64:
            switch (instruction->raw.mvex.SSS)
            {
            case 0:
                break;
            case 1:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_1_TO_8;
                break;
            case 2:
                instruction->avx.broadcast.mode = ZYDIS_BROADCAST_MODE_4_TO_8;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_UF_32:
        case ZYDIS_MVEX_FUNC_DF_32:
            switch (instruction->raw.mvex.SSS)
            {
            case 0:
                break;
            case 3:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_FLOAT16;
                break;
            case 4:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_UF_64:
        case ZYDIS_MVEX_FUNC_DF_64:
            break;
        case ZYDIS_MVEX_FUNC_UI_32:
        case ZYDIS_MVEX_FUNC_DI_32:
            switch (instruction->raw.mvex.SSS)
            {
            case 0:
                break;
            case 4:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT8;
                break;
            case 5:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT8;
                break;
            case 6:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_UINT16;
                break;
            case 7:
                instruction->avx.conversion.mode = ZYDIS_CONVERSION_MODE_SINT16;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
            break;
        case ZYDIS_MVEX_FUNC_UI_64:
        case ZYDIS_MVEX_FUNC_DI_64:
            break;
        default:
            ZYAN_UNREACHABLE;
        }

        // Eviction hint
        if ((instruction->raw.modrm.mod != 3) && instruction->raw.mvex.E)
        {
            instruction->avx.has_eviction_hint = ZYAN_TRUE;
        }

        // Mask
        instruction->avx.mask.mode = ZYDIS_MASK_MODE_MERGING;
        instruction->avx.mask.reg = ZYDIS_REGISTER_K0 + instruction->raw.mvex.kkk;
#else
        ZYAN_UNREACHABLE;
#endif
        break;
    }
    default:
        // Nothing to do here
        break;
    }
}
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Physical instruction decoding                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Collects optional instruction prefixes.
 *
 * @param   state     A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 *
 * @return  A zyan status code.
 *
 * This function sets the corresponding flag for each prefix and automatically decodes the last
 * `REX`-prefix (if exists).
 */
static ZyanStatus ZydisCollectOptionalPrefixes(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(instruction->raw.prefix_count == 0);

    ZyanU8 rex = 0x00;
    ZyanU8 offset = 0;
    ZyanBool done = ZYAN_FALSE;
    do
    {
        ZyanU8 prefix_byte;
        ZYAN_CHECK(ZydisInputPeek(state, instruction, &prefix_byte));
        switch (prefix_byte)
        {
        case 0xF0:
            state->prefixes.has_lock = ZYAN_TRUE;
            state->prefixes.offset_lock = offset;
            break;
        case 0xF2:
            ZYAN_FALLTHROUGH;
        case 0xF3:
            state->prefixes.group1 = prefix_byte;
            state->prefixes.mandatory_candidate = prefix_byte;
            state->prefixes.offset_group1 = offset;
            state->prefixes.offset_mandatory = offset;
            break;
        case 0x2E:
            ZYAN_FALLTHROUGH;
        case 0x36:
            ZYAN_FALLTHROUGH;
        case 0x3E:
            ZYAN_FALLTHROUGH;
        case 0x26:
            if (state->decoder->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
            {
                if ((prefix_byte == 0x3E) &&
                    (state->prefixes.effective_segment != 0x64) &&
                    (state->prefixes.effective_segment != 0x65))
                {
                    state->prefixes.offset_notrack = offset;
                }
                state->prefixes.group2 = prefix_byte;
                state->prefixes.offset_group2 = offset;
                break;
            }
            ZYAN_FALLTHROUGH;
        case 0x64:
            ZYAN_FALLTHROUGH;
        case 0x65:
            state->prefixes.group2 = prefix_byte;
            state->prefixes.offset_group2 = offset;
            state->prefixes.effective_segment = prefix_byte;
            state->prefixes.offset_segment = offset;
            state->prefixes.offset_notrack = -1;
            break;
        case 0x66:
            // context->prefixes.has_osz_override = ZYAN_TRUE;
            state->prefixes.offset_osz_override = offset;
            if (!state->prefixes.mandatory_candidate)
            {
                state->prefixes.mandatory_candidate = 0x66;
                state->prefixes.offset_mandatory = offset;
            }
            instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
            break;
        case 0x67:
            // context->prefixes.has_asz_override = ZYAN_TRUE;
            state->prefixes.offset_asz_override = offset;
            instruction->attributes |= ZYDIS_ATTRIB_HAS_ADDRESSSIZE;
            break;
        default:
            if ((state->decoder->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
                (prefix_byte & 0xF0) == 0x40)
            {
                rex = prefix_byte;
                instruction->raw.rex.offset = offset;
            } else
            {
                done = ZYAN_TRUE;
            }
            break;
        }
        if (!done)
        {
            // Invalidate `REX`, if it's not the last legacy prefix
            if (rex && (rex != prefix_byte))
            {
                rex = 0x00;
                instruction->raw.rex.offset = 0;
            }
            instruction->raw.prefixes[instruction->raw.prefix_count++].value = prefix_byte;
            ZydisInputSkip(state, instruction);
            ++offset;
        }
    } while (!done);

    if (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE)
    {
        instruction->raw.prefixes[state->prefixes.offset_osz_override].type =
            ZYDIS_PREFIX_TYPE_EFFECTIVE;
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE)
    {
        instruction->raw.prefixes[state->prefixes.offset_asz_override].type =
            ZYDIS_PREFIX_TYPE_EFFECTIVE;
    }
    if (rex)
    {
        instruction->raw.prefixes[instruction->raw.rex.offset].type = ZYDIS_PREFIX_TYPE_EFFECTIVE;
        ZydisDecodeREX(state->context, instruction, rex);
    }
    if ((state->decoder->machine_mode != ZYDIS_MACHINE_MODE_LONG_64) &&
        (state->prefixes.group2 == 0x3E))
    {
        state->prefixes.offset_notrack = state->prefixes.offset_group2;
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Decodes optional instruction parts like the ModRM byte, the SIB byte and
 * additional displacements and/or immediate values.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   info        A pointer to the `ZydisInstructionEncodingInfo` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeOptionalInstructionParts(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, const ZydisInstructionEncodingInfo* info)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(info);

    ZydisDecoderContext* context = state->context;

    if (info->flags & ZYDIS_INSTR_ENC_FLAG_HAS_MODRM)
    {
        if (!instruction->raw.modrm.offset)
        {
            instruction->raw.modrm.offset = instruction->length;
            ZyanU8 modrm_byte;
            ZYAN_CHECK(ZydisInputNext(state, instruction, &modrm_byte));
            ZydisDecodeModRM(instruction, modrm_byte);
        }

        if (!(info->flags & ZYDIS_INSTR_ENC_FLAG_FORCE_REG_FORM))
        {
            ZyanU8 has_sib = 0;
            ZyanU8 displacement_size = 0;
            switch (instruction->address_width)
            {
            case 16:
                switch (instruction->raw.modrm.mod)
                {
                case 0:
                    if (instruction->raw.modrm.rm == 6)
                    {
                        displacement_size = 16;
                    }
                    break;
                case 1:
                    displacement_size = 8;
                    break;
                case 2:
                    displacement_size = 16;
                    break;
                case 3:
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
                break;
            case 32:
            case 64:
                has_sib =
                    (instruction->raw.modrm.mod != 3) && (instruction->raw.modrm.rm == 4);
                switch (instruction->raw.modrm.mod)
                {
                case 0:
                    if (instruction->raw.modrm.rm == 5)
                    {
                        if (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
                        {
                            instruction->attributes |= ZYDIS_ATTRIB_IS_RELATIVE;
                        }
                        displacement_size = 32;
                    }
                    break;
                case 1:
                    displacement_size = 8;
                    break;
                case 2:
                    displacement_size = 32;
                    break;
                case 3:
                    break;
                default:
                    ZYAN_UNREACHABLE;
                }
                break;
            default:
                ZYAN_UNREACHABLE;
            }
            if (has_sib)
            {
                instruction->raw.sib.offset = instruction->length;
                ZyanU8 sib_byte;
                ZYAN_CHECK(ZydisInputNext(state, instruction, &sib_byte));
                ZydisDecodeSIB(instruction, sib_byte);
                if (instruction->raw.sib.base == 5)
                {
                    displacement_size = (instruction->raw.modrm.mod == 1) ? 8 : 32;
                }
            }
            if (displacement_size)
            {
                ZYAN_CHECK(ZydisReadDisplacement(state, instruction, displacement_size));
            }
        }

        context->reg_info.is_mod_reg = (instruction->raw.modrm.mod == 3) ||
                                       (info->flags & ZYDIS_INSTR_ENC_FLAG_FORCE_REG_FORM);
    }

    if (info->flags & ZYDIS_INSTR_ENC_FLAG_HAS_DISP)
    {
        ZYAN_CHECK(ZydisReadDisplacement(
            state, instruction, info->disp.size[context->easz_index]));
    }

    if (info->flags & ZYDIS_INSTR_ENC_FLAG_HAS_IMM0)
    {
        if (info->imm[0].is_relative)
        {
            instruction->attributes |= ZYDIS_ATTRIB_IS_RELATIVE;
        }
        ZYAN_CHECK(ZydisReadImmediate(state, instruction, 0,
            info->imm[0].size[context->eosz_index], info->imm[0].is_signed,
            info->imm[0].is_relative));
    }

    if (info->flags & ZYDIS_INSTR_ENC_FLAG_HAS_IMM1)
    {
        ZYAN_ASSERT(!(info->flags & ZYDIS_INSTR_ENC_FLAG_HAS_DISP));
        ZYAN_CHECK(ZydisReadImmediate(state, instruction, 1,
            info->imm[1].size[context->eosz_index], info->imm[1].is_signed,
            info->imm[1].is_relative));
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * Sets the effective operand size for the given instruction.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   definition  A pointer to the `ZydisInstructionDefinition` struct.
 */
static void ZydisSetEffectiveOperandWidth(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(definition);

    static const ZyanU8 operand_size_map[8][8] =
    {
        // Default for most instructions
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            32, // 64 __ W0
            16, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 8-bit (this is done later to preserve the `eosz_index`)
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            32, // 64 __ W0
            16, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size override 0x66 is ignored
        {
            16, // 16 __ W0
            16, // 16 66 W0
            32, // 32 __ W0
            32, // 32 66 W0
            32, // 64 __ W0
            32, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // REX.W promotes to 32-bit instead of 64-bit
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            32, // 64 __ W0
            16, // 64 66 W0
            32, // 64 __ W1
            32  // 64 66 W1
        },
        // Operand size defaults to 64-bit in 64-bit mode
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            64, // 64 __ W0
            16, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 64-bit in 64-bit mode
        {
            16, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            16, // 32 66 W0
            64, // 64 __ W0
            64, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 32-bit, if no REX.W is present.
        {
            32, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            32, // 32 66 W0
            32, // 64 __ W0
            32, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        },
        // Operand size is forced to 64-bit in 64-bit mode and forced to 32-bit in all other modes.
        // This is used for e.g. `mov CR, GPR` and `mov GPR, CR`.
        {
            32, // 16 __ W0
            32, // 16 66 W0
            32, // 32 __ W0
            32, // 32 66 W0
            64, // 64 __ W0
            64, // 64 66 W0
            64, // 64 __ W1
            64  // 64 66 W1
        }
    };

    ZyanU8 index = (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) ? 1 : 0;
    if ((instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_COMPAT_32) ||
        (instruction->machine_mode == ZYDIS_MACHINE_MODE_LEGACY_32))
    {
        index += 2;
    }
    else if (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
    {
        index += 4;
        index += (context->vector_unified.W & 0x01) << 1;
    }

    ZYAN_ASSERT(definition->operand_size_map < ZYAN_ARRAY_LENGTH(operand_size_map));
    ZYAN_ASSERT(index < ZYAN_ARRAY_LENGTH(operand_size_map[definition->operand_size_map]));

    instruction->operand_width = operand_size_map[definition->operand_size_map][index];
    context->eosz_index = instruction->operand_width >> 5;

    // TODO: Cleanup code and remove hardcoded condition
    if (definition->operand_size_map == 1)
    {
        instruction->operand_width = 8;
    }
}

/**
 * Sets the effective address width for the given instruction.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   definition  A pointer to the `ZydisInstructionDefinition` struct.
 */
static void ZydisSetEffectiveAddressWidth(ZydisDecoderContext* context,
    ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);

    static const ZyanU8 address_size_map[3][8] =
    {
        // Default for most instructions
        {
            16, // 16 __
            32, // 16 67
            32, // 32 __
            16, // 32 67
            64, // 64 __
            32  // 64 67
        },
        // The address-size override is ignored
        {
            16, // 16 __
            16, // 16 67
            32, // 32 __
            32, // 32 67
            64, // 64 __
            64  // 64 67
        },
        // The address-size is forced to 64-bit in 64-bit mode and 32-bit in non 64-bit mode. This
        // is used by e.g. `ENCLS`, `ENCLV`, `ENCLU`.
        {
            32, // 16 __
            32, // 16 67
            32, // 32 __
            32, // 32 67
            64, // 64 __
            64  // 64 67
        }
    };

    ZyanU8 index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 1 : 0;
    if ((instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_COMPAT_32) ||
        (instruction->machine_mode == ZYDIS_MACHINE_MODE_LEGACY_32))
    {
        index += 2;
    }
    else if (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
    {
        index += 4;
    }

    ZYAN_ASSERT(definition->address_size_map < ZYAN_ARRAY_LENGTH(address_size_map));
    ZYAN_ASSERT(index < ZYAN_ARRAY_LENGTH(address_size_map[definition->address_size_map]));

    instruction->address_width = address_size_map[definition->address_size_map][index];
    context->easz_index = instruction->address_width >> 5;
}

/* ---------------------------------------------------------------------------------------------- */

static ZyanStatus ZydisNodeHandlerXOP(const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        *index = 0;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_XOP);
        *index = (instruction->raw.xop.m_mmmm - 0x08) + (instruction->raw.xop.pp * 3) + 1;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerVEX(const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        *index = 0;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_VEX);
        *index = instruction->raw.vex.m_mmmm + (instruction->raw.vex.pp << 2) + 1;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerEMVEX(const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        *index = 0;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX);
        *index = instruction->raw.evex.mmm + (instruction->raw.evex.pp << 3) + 1;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MVEX);
        *index = instruction->raw.mvex.mmmm + (instruction->raw.mvex.pp << 2) + 33;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerOpcode(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    // Handle possible encoding-prefix and opcode-map changes
    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        ZYAN_CHECK(ZydisInputNext(state, instruction, &instruction->opcode));
        switch (instruction->opcode_map)
        {
        case ZYDIS_OPCODE_MAP_DEFAULT:
            switch (instruction->opcode)
            {
            case 0x0F:
                instruction->opcode_map = ZYDIS_OPCODE_MAP_0F;
                break;
            case 0xC4:
            case 0xC5:
            case 0x62:
            {
                ZyanU8 next_input;
                ZYAN_CHECK(ZydisInputPeek(state, instruction, &next_input));
                if (((next_input & 0xF0) >= 0xC0) ||
                    (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64))
                {
                    if (instruction->attributes & ZYDIS_ATTRIB_HAS_REX)
                    {
                        return ZYDIS_STATUS_ILLEGAL_REX;
                    }
                    if (state->prefixes.has_lock)
                    {
                        return ZYDIS_STATUS_ILLEGAL_LOCK;
                    }
                    if (state->prefixes.mandatory_candidate)
                    {
                        return ZYDIS_STATUS_ILLEGAL_LEGACY_PFX;
                    }
                    ZyanU8 prefix_bytes[4] = { 0, 0, 0, 0 };
                    prefix_bytes[0] = instruction->opcode;
                    switch (instruction->opcode)
                    {
                    case 0xC4:
                        instruction->raw.vex.offset = instruction->length - 1;
                        // Read additional 3-byte VEX-prefix data
                        ZYAN_ASSERT(!(instruction->attributes & ZYDIS_ATTRIB_HAS_VEX));
                        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, &prefix_bytes[1], 2));
                        break;
                    case 0xC5:
                        instruction->raw.vex.offset = instruction->length - 1;
                        // Read additional 2-byte VEX-prefix data
                        ZYAN_ASSERT(!(instruction->attributes & ZYDIS_ATTRIB_HAS_VEX));
                        ZYAN_CHECK(ZydisInputNext(state, instruction, &prefix_bytes[1]));
                        break;
                    case 0x62:
#if !defined(ZYDIS_DISABLE_AVX512) || !defined(ZYDIS_DISABLE_KNC)
                        // Read additional EVEX/MVEX-prefix data
                        ZYAN_ASSERT(!(instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX));
                        ZYAN_ASSERT(!(instruction->attributes & ZYDIS_ATTRIB_HAS_MVEX));
                        ZYAN_CHECK(ZydisInputNextBytes(state, instruction, &prefix_bytes[1], 3));
                        break;
#else
                        return ZYDIS_STATUS_DECODING_ERROR;
#endif
                    default:
                        ZYAN_UNREACHABLE;
                    }
                    switch (instruction->opcode)
                    {
                    case 0xC4:
                    case 0xC5:
                        // Decode VEX-prefix
                        instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_VEX;
                        ZYAN_CHECK(ZydisDecodeVEX(state->context, instruction, prefix_bytes));
                        instruction->opcode_map =
                            ZYDIS_OPCODE_MAP_DEFAULT + instruction->raw.vex.m_mmmm;
                        break;
                    case 0x62:
#if defined(ZYDIS_DISABLE_AVX512) && defined(ZYDIS_DISABLE_KNC)
                        return ZYDIS_STATUS_DECODING_ERROR;
#else
                        switch ((prefix_bytes[2] >> 2) & 0x01)
                        {
                        case 0:
#ifndef ZYDIS_DISABLE_KNC
                            instruction->raw.mvex.offset = instruction->length - 4;
                            // `KNC` instructions are only valid in 64-bit mode.
                            // This condition catches the `MVEX` encoded ones to save a bunch of
                            // `mode` filters in the data-tables.
                            // `KNC` instructions with `VEX` encoding still require a `mode` filter.
                            if (state->decoder->machine_mode != ZYDIS_MACHINE_MODE_LONG_64)
                            {
                                return ZYDIS_STATUS_DECODING_ERROR;
                            }
                            // Decode MVEX-prefix
                            instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_MVEX;
                            ZYAN_CHECK(ZydisDecodeMVEX(state->context, instruction, prefix_bytes));
                            instruction->opcode_map =
                                ZYDIS_OPCODE_MAP_DEFAULT + instruction->raw.mvex.mmmm;
                            break;
#else
                            return ZYDIS_STATUS_DECODING_ERROR;
#endif
                        case 1:
#ifndef ZYDIS_DISABLE_AVX512
                            instruction->raw.evex.offset = instruction->length - 4;
                            // Decode EVEX-prefix
                            instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_EVEX;
                            ZYAN_CHECK(ZydisDecodeEVEX(state->context, instruction, prefix_bytes));
                            instruction->opcode_map =
                                ZYDIS_OPCODE_MAP_DEFAULT + instruction->raw.evex.mmm;
                            break;
#else
                            return ZYDIS_STATUS_DECODING_ERROR;
#endif
                        default:
                            ZYAN_UNREACHABLE;
                        }
                        break;
#endif
                    default:
                        ZYAN_UNREACHABLE;
                    }
                }
                break;
            }
            case 0x8F:
            {
                ZyanU8 next_input;
                ZYAN_CHECK(ZydisInputPeek(state, instruction, &next_input));
                if ((next_input & 0x1F) >= 8)
                {
                    if (instruction->attributes & ZYDIS_ATTRIB_HAS_REX)
                    {
                        return ZYDIS_STATUS_ILLEGAL_REX;
                    }
                    if (state->prefixes.has_lock)
                    {
                        return ZYDIS_STATUS_ILLEGAL_LOCK;
                    }
                    if (state->prefixes.mandatory_candidate)
                    {
                        return ZYDIS_STATUS_ILLEGAL_LEGACY_PFX;
                    }
                    instruction->raw.xop.offset = instruction->length - 1;
                    ZyanU8 prefixBytes[3] = { 0x8F, 0x00, 0x00 };
                    // Read additional xop-prefix data
                    ZYAN_CHECK(ZydisInputNextBytes(state, instruction, &prefixBytes[1], 2));
                    // Decode xop-prefix
                    instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_XOP;
                    ZYAN_CHECK(ZydisDecodeXOP(state->context, instruction, prefixBytes));
                    instruction->opcode_map =
                        ZYDIS_OPCODE_MAP_XOP8 + instruction->raw.xop.m_mmmm - 0x08;
                }
                break;
            }
            default:
                break;
            }
            break;
        case ZYDIS_OPCODE_MAP_0F:
            switch (instruction->opcode)
            {
            case 0x0F:
                if (state->prefixes.has_lock)
                {
                    return ZYDIS_STATUS_ILLEGAL_LOCK;
                }
                instruction->encoding = ZYDIS_INSTRUCTION_ENCODING_3DNOW;
                instruction->opcode_map = ZYDIS_OPCODE_MAP_0F0F;
                break;
            case 0x38:
                instruction->opcode_map = ZYDIS_OPCODE_MAP_0F38;
                break;
            case 0x3A:
                instruction->opcode_map = ZYDIS_OPCODE_MAP_0F3A;
                break;
            default:
                break;
            }
            break;
        case ZYDIS_OPCODE_MAP_0F38:
        case ZYDIS_OPCODE_MAP_0F3A:
        case ZYDIS_OPCODE_MAP_XOP8:
        case ZYDIS_OPCODE_MAP_XOP9:
        case ZYDIS_OPCODE_MAP_XOPA:
            // Nothing to do here
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        break;
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
        // All 3DNOW (0x0F 0x0F) instructions are using the same operand encoding. We just
        // decode a random (pi2fw) instruction and extract the actual opcode later.
        *index = 0x0C;
        return ZYAN_STATUS_SUCCESS;
    default:
        ZYAN_CHECK(ZydisInputNext(state, instruction, &instruction->opcode));
        break;
    }

    *index = instruction->opcode;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerMode(const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->machine_mode)
    {
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
    case ZYDIS_MACHINE_MODE_LEGACY_16:
    case ZYDIS_MACHINE_MODE_REAL_16:
        *index = 0;
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
    case ZYDIS_MACHINE_MODE_LEGACY_32:
        *index = 1;
        break;
    case ZYDIS_MACHINE_MODE_LONG_64:
        *index = 2;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerModeCompact(const ZydisDecodedInstruction* instruction,
    ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    *index = (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) ? 0 : 1;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerModrmMod(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    if (!instruction->raw.modrm.offset)
    {
        instruction->raw.modrm.offset = instruction->length;
        ZyanU8 modrm_byte;
        ZYAN_CHECK(ZydisInputNext(state, instruction, &modrm_byte));
        ZydisDecodeModRM(instruction, modrm_byte);
    }
    *index = instruction->raw.modrm.mod;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerModrmModCompact(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_CHECK(ZydisNodeHandlerModrmMod(state, instruction, index));
    *index = (*index == 0x3) ? 0 : 1;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerModrmReg(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    if (!instruction->raw.modrm.offset)
    {
        instruction->raw.modrm.offset = instruction->length;
        ZyanU8 modrm_byte;
        ZYAN_CHECK(ZydisInputNext(state, instruction, &modrm_byte));
        ZydisDecodeModRM(instruction, modrm_byte);
    }
    *index = instruction->raw.modrm.reg;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerModrmRm(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    if (!instruction->raw.modrm.offset)
    {
        instruction->raw.modrm.offset = instruction->length;
        ZyanU8 modrm_byte;
        ZYAN_CHECK(ZydisInputNext(state, instruction, &modrm_byte));
        ZydisDecodeModRM(instruction, modrm_byte);
    }
    *index = instruction->raw.modrm.rm;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerMandatoryPrefix(const ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (state->prefixes.mandatory_candidate)
    {
    case 0x66:
        instruction->raw.prefixes[state->prefixes.offset_mandatory].type =
            ZYDIS_PREFIX_TYPE_MANDATORY;
        instruction->attributes &= ~ZYDIS_ATTRIB_HAS_OPERANDSIZE;
        *index = 2;
        break;
    case 0xF3:
        instruction->raw.prefixes[state->prefixes.offset_mandatory].type =
            ZYDIS_PREFIX_TYPE_MANDATORY;
        *index = 3;
        break;
    case 0xF2:
        instruction->raw.prefixes[state->prefixes.offset_mandatory].type =
            ZYDIS_PREFIX_TYPE_MANDATORY;
        *index = 4;
        break;
    default:
        *index = 1;
        break;
    }
    // TODO: Consume prefix and make sure it's available again, if we need to fallback

    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerOperandSize(const ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    if ((instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
        (state->context->vector_unified.W))
    {
        *index = 2;
    } else
    {
        if (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE)
        {
            instruction->raw.prefixes[state->prefixes.offset_osz_override].type =
                ZYDIS_PREFIX_TYPE_EFFECTIVE;
        }
        switch (instruction->machine_mode)
        {
        case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
        case ZYDIS_MACHINE_MODE_LEGACY_16:
        case ZYDIS_MACHINE_MODE_REAL_16:
            *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) ? 1 : 0;
            break;
        case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
        case ZYDIS_MACHINE_MODE_LEGACY_32:
        case ZYDIS_MACHINE_MODE_LONG_64:
            *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE) ? 0 : 1;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
    }

    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerAddressSize(ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    /*if (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE)
    {
        instruction->raw.prefixes[context->prefixes.offset_asz_override].type =
            ZYDIS_PREFIX_TYPE_EFFECTIVE;
    }*/
    switch (instruction->machine_mode)
    {
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
    case ZYDIS_MACHINE_MODE_LEGACY_16:
    case ZYDIS_MACHINE_MODE_REAL_16:
        *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 1 : 0;
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
    case ZYDIS_MACHINE_MODE_LEGACY_32:
        *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 0 : 1;
        break;
    case ZYDIS_MACHINE_MODE_LONG_64:
        *index = (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE) ? 1 : 2;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerVectorLength(const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_XOP);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_VEX);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MVEX);
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    *index = context->vector_unified.LL;
    if (*index == 3)
    {
        return ZYDIS_STATUS_DECODING_ERROR;
    }
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerRexW(const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        // nothing to do here
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_XOP);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_VEX);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MVEX);
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    *index = context->vector_unified.W;
    return ZYAN_STATUS_SUCCESS;
}

static ZyanStatus ZydisNodeHandlerRexB(const ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        // nothing to do here
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_XOP);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_VEX);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX);
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MVEX);
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    *index = context->vector_unified.B;
    return ZYAN_STATUS_SUCCESS;
}

#ifndef ZYDIS_DISABLE_AVX512
static ZyanStatus ZydisNodeHandlerEvexB(const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    ZYAN_ASSERT(instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX);
    ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX);
    *index = instruction->raw.evex.b;
    return ZYAN_STATUS_SUCCESS;
}
#endif

#ifndef ZYDIS_DISABLE_KNC
static ZyanStatus ZydisNodeHandlerMvexE(const ZydisDecodedInstruction* instruction, ZyanU16* index)
{
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(index);

    ZYAN_ASSERT(instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX);
    ZYAN_ASSERT(instruction->attributes & ZYDIS_ATTRIB_HAS_MVEX);
    *index = instruction->raw.mvex.E;
    return ZYAN_STATUS_SUCCESS;
}
#endif

/* ---------------------------------------------------------------------------------------------- */

/**
 * Populates the internal register id fields for `REG`, `RM`, `NDSNDD`, `BASE` and `INDEX`/`VIDX`
 * encoded operands and performs sanity checks.
 *
 * @param   context     A pointer to the `ZydisDecoderContext` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   def_reg     The type definition for the `.reg` encoded operand.
 * @param   def_rm      The type definition for the `.rm` encoded operand.
 * @param   def_ndsndd  The type definition for the `.vvvv` encoded operand.
 *
 * @return  A zyan status code.
 *
 * This function sets all unused register ids to `-1`. This rule does currently not apply to
 * `base` and `index`.
 *
 * Definition encoding:
 * - `def_reg`    -> `ZydisRegisterKind`
 * - `def_ndsndd` -> `ZydisRegisterKind`
 * - `def_rm`     -> `ZydisRegisterKind` (`.mod == 3`) or ZydisMemoryOperandType (`.mod != 3`)
 */
static ZyanStatus ZydisPopulateRegisterIds(ZydisDecoderContext* context,
    const ZydisDecodedInstruction* instruction, ZyanU8 def_reg, ZyanU8 def_rm, ZyanU8 def_ndsndd)
{
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(instruction);

    const ZyanBool is_64_bit = (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64);
    const ZyanBool is_reg    = context->reg_info.is_mod_reg;
    const ZyanBool has_sib   = !is_reg && (instruction->raw.modrm.rm == 4);
    const ZyanBool has_vsib  = has_sib && (def_rm == ZYDIS_MEMOP_TYPE_VSIB);

    ZyanU8 id_reg    = instruction->raw.modrm.reg;
    ZyanU8 id_rm     = instruction->raw.modrm.rm;
    ZyanU8 id_ndsndd = is_64_bit ? context->vector_unified.vvvv : context->vector_unified.vvvv & 0x07;
    ZyanU8 id_base   = has_sib ? instruction->raw.sib.base : instruction->raw.modrm.rm;
    ZyanU8 id_index  = instruction->raw.sib.index;

    if (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
    {
        const ZyanBool is_emvex = (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                                  (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX);

        // The `index` extension by `.v'` is only valid for VSIB operands
        const ZyanU8 vsib_v2 = has_vsib ? context->vector_unified.V2 : 0;
        // The `rm` extension by `.X` is only valid for EVEX/MVEX instructions
        const ZyanU8 evex_x  = is_emvex ? context->vector_unified.X  : 0;

        id_reg    |= (context->vector_unified.R2 << 4) | (context->vector_unified.R << 3);
        id_rm     |= (evex_x                     << 4) | (context->vector_unified.B << 3);
        id_ndsndd |= (context->vector_unified.V2 << 4)                                   ;
        id_base   |=                                     (context->vector_unified.B << 3);
        id_index  |= (vsib_v2                    << 4) | (context->vector_unified.X << 3);

        // The masking emulates the actual CPU behavior and does not verify if the resulting ids
        // are actually valid for the given register kind.

        static const ZyanU8 mask_reg[ZYDIS_REGKIND_MAX_VALUE + 1] =
        {
            /* INVALID */ 0,
            /* GPR     */ (1 << 5) - 1,
            /* X87     */ (1 << 3) - 1, // ignore `.R`, ignore `.R'`
            /* MMX     */ (1 << 3) - 1, // ignore `.R`, ignore `.R'`
            /* VR      */ (1 << 5) - 1,
            /* TMM     */ (1 << 5) - 1,
            /* SEGMENT */ (1 << 3) - 1, // ignore `.R`, ignore `.R'`
            /* TEST    */ (1 << 3) - 1, // ignore `.R`, ignore `.R'`
            /* CONTROL */ (1 << 4) - 1, //              ignore `.R'`
            /* DEBUG   */ (1 << 4) - 1, //              ignore `.R'`
            /* MASK    */ (1 << 5) - 1,
            /* BOUND   */ (1 << 4) - 1  //              ignore `.R'`
        };
        id_reg &= mask_reg[def_reg];

        static const ZyanU8 mask_rm[ZYDIS_REGKIND_MAX_VALUE + 1] =
        {
            /* INVALID */ 0,
            /* GPR     */ (1 << 4) - 1, //              ignore `.X`
            /* X87     */ (1 << 3) - 1, // ignore `.B`, ignore `.X`
            /* MMX     */ (1 << 3) - 1, // ignore `.B`, ignore `.X`
            /* VR      */ (1 << 5) - 1,
            /* TMM     */ (1 << 4) - 1, //              ignore `.X`
            /* SEGMENT */ (1 << 3) - 1, // ignore `.B`, ignore `.X`
            /* TEST    */ (1 << 3) - 1, // ignore `.B`, ignore `.X`
            /* CONTROL */ (1 << 4) - 1, //              ignore `.X`
            /* DEBUG   */ (1 << 4) - 1, //              ignore `.X`
            /* MASK    */ (1 << 3) - 1, // ignore `.B`, ignore `.X`
            /* BOUND   */ (1 << 4) - 1  //              ignore `.X`
        };
        id_rm &= (is_reg ? mask_rm[def_rm] : 0xFF);

        // Commented out for future reference. Not required at the moment as it's always either
        // a "take all" or "take nothing" situation.

        //static const ZyanU8 mask_ndsndd[ZYDIS_REGKIND_MAX_VALUE + 1] =
        //{
        //    /* INVALID */ 0,
        //    /* GPR     */ (1 << 5) - 1,
        //    /* X87     */ 0,            // never encoded in `.vvvv`
        //    /* MMX     */ 0,            // never encoded in `.vvvv`
        //    /* VR      */ (1 << 5) - 1,
        //    /* TMM     */ (1 << 5) - 1,
        //    /* SEGMENT */ 0,            // never encoded in `.vvvv`
        //    /* TEST    */ 0,            // never encoded in `.vvvv`
        //    /* CONTROL */ 0,            // never encoded in `.vvvv`
        //    /* DEBUG   */ 0,            // never encoded in `.vvvv`
        //    /* MASK    */ (1 << 5) - 1,
        //    /* BOUND   */ 0             // never encoded in `.vvvv`
        //};
    }

    // Validate

    // `.vvvv` is not allowed, if the instruction does not encode a NDS/NDD operand
    if (!def_ndsndd && context->vector_unified.vvvv)
    {
        return ZYDIS_STATUS_BAD_REGISTER;
    }
    // `.v'` is not allowed, if the instruction does not encode a NDS/NDD or VSIB operand
    if (!def_ndsndd && !has_vsib && context->vector_unified.V2)
    {
        return ZYDIS_STATUS_BAD_REGISTER;
    }

    static const ZyanU8 available_regs[2][ZYDIS_REGKIND_MAX_VALUE + 1] =
    {
        // 16/32 bit mode
        {
            /* INVALID */ 255,
            /* GPR     */   8,
            /* X87     */   8,
            /* MMX     */   8,
            /* VR      */   8,
            /* TMM     */   8,
            /* SEGMENT */   6,
            /* TEST    */   8,
            /* CONTROL */   8,
            /* DEBUG   */   8,
            /* MASK    */   8,
            /* BOUND   */   4
        },
        // 64 bit mode
        {
            /* INVALID */ 255,
            /* GPR     */  16,
            /* X87     */   8,
            /* MMX     */   8,
            /* VR      */  32,
            /* TMM     */   8,
            /* SEGMENT */   6,
            /* TEST    */   8,
            /* CONTROL */  16,
            // Attempts to reference DR8..DR15 result in undefined opcode (#UD) exceptions. DR4 and
            // DR5 are only valid, if the debug extension (DE) flag in CR4 is set. As we can't
            // check this at runtime we just allow them.
            /* DEBUG   */   8,
            /* MASK    */   8,
            /* BOUND   */   4
        }
    };

    if ((id_reg >= available_regs[is_64_bit][def_reg]) ||
        (id_ndsndd >= available_regs[is_64_bit][def_ndsndd]) ||
        (is_reg && (id_rm >= available_regs[is_64_bit][def_rm])))
    {
        return ZYDIS_STATUS_BAD_REGISTER;
    }

    ZyanI8 id_cr = -1;
    if (def_reg == ZYDIS_REGKIND_CONTROL)
    {
        id_cr = id_reg;
    }
    if (is_reg && (def_rm == ZYDIS_REGKIND_CONTROL))
    {
        id_cr = id_rm;
    }
    if (id_cr >= 0)
    {
        // Attempts to reference CR1, CR5, CR6, CR7, and CR9..CR15 result in undefined opcode (#UD)
        // exceptions
        static const ZyanU8 lookup[16] =
        {
            1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0
        };
        ZYAN_ASSERT((ZyanUSize)id_cr < ZYAN_ARRAY_LENGTH(lookup));
        if (!lookup[id_cr])
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
    }

    // Assign to context

    context->reg_info.id_reg    = def_reg          ? id_reg    : -1;
    context->reg_info.id_rm     = def_rm && is_reg ? id_rm     : -1;
    context->reg_info.id_ndsndd = def_ndsndd       ? id_ndsndd : -1;
    context->reg_info.id_base   = id_base;  // TODO: Set unused register to -1 as well
    context->reg_info.id_index  = id_index; // TODO: Set unused register to -1 as well

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Checks for certain post-decode error-conditions.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   definition  A pointer to the `ZydisInstructionDefinition` struct.
 *
 * @return  A zyan status code.
 *
 * This function is called immediately after a valid instruction-definition was found.
 */
static ZyanStatus ZydisCheckErrorConditions(ZydisDecoderState* state,
    const ZydisDecodedInstruction* instruction, const ZydisInstructionDefinition* definition)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);
    ZYAN_ASSERT(definition);

    ZyanU8 def_reg                  = definition->op_reg;
    ZyanU8 def_rm                   = definition->op_rm;
    ZyanU8 def_ndsndd               = ZYDIS_REGKIND_INVALID;
    ZyanBool is_gather              = ZYAN_FALSE;
    ZyanBool no_source_dest_match   = ZYAN_FALSE;
    ZyanBool no_source_source_match = ZYAN_FALSE;
#if !defined(ZYDIS_DISABLE_AVX512) || !defined(ZYDIS_DISABLE_KNC)
    ZydisMaskPolicy mask_policy     = ZYDIS_MASK_POLICY_INVALID;
#endif

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
    {
        const ZydisInstructionDefinitionLEGACY* def =
            (const ZydisInstructionDefinitionLEGACY*)definition;

        if (def->requires_protected_mode &&
            (instruction->machine_mode == ZYDIS_MACHINE_MODE_REAL_16))
        {
            return ZYDIS_STATUS_DECODING_ERROR;
        }

        if (def->no_compat_mode &&
            ((instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_COMPAT_16) ||
             (instruction->machine_mode == ZYDIS_MACHINE_MODE_LONG_COMPAT_32)))
        {
            return ZYDIS_STATUS_DECODING_ERROR;
        }

        if (state->prefixes.has_lock && !def->accepts_LOCK)
        {
            return ZYDIS_STATUS_ILLEGAL_LOCK;
        }
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
    {
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    {
        const ZydisInstructionDefinitionXOP* def =
            (const ZydisInstructionDefinitionXOP*)definition;
        def_ndsndd = def->op_ndsndd;
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    {
        const ZydisInstructionDefinitionVEX* def =
            (const ZydisInstructionDefinitionVEX*)definition;
        def_ndsndd             = def->op_ndsndd;
        is_gather              = def->is_gather;
        no_source_source_match = def->no_source_source_match;
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    {
#ifndef ZYDIS_DISABLE_AVX512
        const ZydisInstructionDefinitionEVEX* def =
            (const ZydisInstructionDefinitionEVEX*)definition;
        def_ndsndd           = def->op_ndsndd;
        is_gather            = def->is_gather;
        no_source_dest_match = def->no_source_dest_match;
        mask_policy          = def->mask_policy;

        // Check for invalid zero-mask
        if ((instruction->raw.evex.z) && (!def->accepts_zero_mask))
        {
            return ZYDIS_STATUS_INVALID_MASK; // TODO: Dedicated status code
        }
#else
        ZYAN_UNREACHABLE;
#endif
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
    {
#ifndef ZYDIS_DISABLE_KNC
        const ZydisInstructionDefinitionMVEX* def =
            (const ZydisInstructionDefinitionMVEX*)definition;
        def_ndsndd  = def->op_ndsndd;
        is_gather   = def->is_gather;
        mask_policy = def->mask_policy;

        // Check for invalid MVEX.SSS values
        static const ZyanU8 lookup[26][8] =
        {
            // ZYDIS_MVEX_FUNC_IGNORED
            { 1, 1, 1, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_INVALID
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_RC
            { 1, 1, 1, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_SAE
            { 1, 1, 1, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_F_32
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_I_32
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_F_64
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_I_64
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_SWIZZLE_32
            { 1, 1, 1, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_SWIZZLE_64
            { 1, 1, 1, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_SF_32
            { 1, 1, 1, 1, 1, 0, 1, 1 },
            // ZYDIS_MVEX_FUNC_SF_32_BCST
            { 1, 1, 1, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16
            { 1, 0, 1, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_SF_64
            { 1, 1, 1, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_SI_32
            { 1, 1, 1, 0, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_SI_32_BCST
            { 1, 1, 1, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16
            { 1, 0, 1, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_SI_64
            { 1, 1, 1, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_UF_32
            { 1, 0, 0, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_UF_64
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_UI_32
            { 1, 0, 0, 0, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_UI_64
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_DF_32
            { 1, 0, 0, 1, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_DF_64
            { 1, 0, 0, 0, 0, 0, 0, 0 },
            // ZYDIS_MVEX_FUNC_DI_32
            { 1, 0, 0, 0, 1, 1, 1, 1 },
            // ZYDIS_MVEX_FUNC_DI_64
            { 1, 0, 0, 0, 0, 0, 0, 0 }
        };
        ZYAN_ASSERT(def->functionality < ZYAN_ARRAY_LENGTH(lookup));
        ZYAN_ASSERT(instruction->raw.mvex.SSS < 8);
        if (!lookup[def->functionality][instruction->raw.mvex.SSS])
        {
            return ZYDIS_STATUS_DECODING_ERROR;
        }
#else
        ZYAN_UNREACHABLE;
#endif
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }

    ZydisDecoderContext* context = state->context;
    const ZyanBool is_reg = context->reg_info.is_mod_reg;

    ZyanU8 no_rip_rel     = ZYAN_FALSE;
    ZyanU8 is_sr_dest_reg = ZYAN_FALSE;
    ZyanU8 is_sr_dest_rm  = ZYAN_FALSE;
    if (def_reg)
    {
        is_sr_dest_reg = ZYDIS_OPDEF_GET_REG_HIGH_BIT(def_reg);
        def_reg = ZYDIS_OPDEF_GET_REG(def_reg);
    }
    if (def_rm)
    {
        if (is_reg)
        {
            is_sr_dest_rm = ZYDIS_OPDEF_GET_REG_HIGH_BIT(def_rm);
            def_rm = ZYDIS_OPDEF_GET_REG(def_rm);
        }
        else
        {
            no_rip_rel = ZYDIS_OPDEF_GET_MEM_HIGH_BIT(def_rm);
            def_rm = ZYDIS_OPDEF_GET_MEM(def_rm);
        }
    }

    // Check RIP-relative memory addressing
    if (no_rip_rel)
    {
        const ZyanBool is_rip_rel =
            (state->decoder->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
            (instruction->raw.modrm.mod == 0) && (instruction->raw.modrm.rm == 5);
        if (is_rip_rel)
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
    }

    // Populate- and validate register constraints
    ZYAN_CHECK(ZydisPopulateRegisterIds(context, instruction, def_reg, def_rm, def_ndsndd));

    // `ZYDIS_REGISTER_CS` is not allowed as `MOV` target
    if (is_sr_dest_reg && (context->reg_info.id_reg == 1))
    {
        return ZYDIS_STATUS_BAD_REGISTER;
    }
    if (is_sr_dest_rm && (context->reg_info.id_rm == 1))
    {
        return ZYDIS_STATUS_BAD_REGISTER;
    }

    // Check gather registers
    if (is_gather)
    {
        // ZYAN_ASSERT(has_VSIB);
        ZYAN_ASSERT(instruction->raw.modrm.mod != 3);
        ZYAN_ASSERT(instruction->raw.modrm.rm  == 4);

        const ZyanU8 index = context->reg_info.id_index;
        ZyanU8 dest        = context->reg_info.id_reg;
        ZyanU8 mask        = 0xF0;

        if (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX)
        {
            ZYAN_ASSERT((def_reg    == ZYDIS_REGKIND_VR) &&
                        (def_rm     == ZYDIS_MEMOP_TYPE_VSIB) &&
                        (def_ndsndd == ZYDIS_REGKIND_VR));
            mask = context->reg_info.id_ndsndd;
        }

        if ((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
            (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX))
        {
            ZYAN_ASSERT(((def_reg    == ZYDIS_REGKIND_INVALID) ||
                         (def_reg    == ZYDIS_REGKIND_VR)) &&
                         (def_rm     == ZYDIS_MEMOP_TYPE_VSIB) &&
                         (def_ndsndd == ZYDIS_REGKIND_INVALID));

            // Some gather instructions (like `VGATHERPF0{D|Q}{PS|PD}`) do not have a destination
            // operand
            if (!def_reg)
            {
                dest = 0xF1;
            }
        }

        // If any pair of the index, mask, or destination registers are the same, the instruction
        // results a UD fault
        if ((dest == index) || (dest == mask) || (index == mask))
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
    }

    // Check if any source register matches the destination register
    if (no_source_dest_match)
    {
        ZYAN_ASSERT((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                    (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX));

        const ZyanU8 dest    = context->reg_info.id_reg;
        const ZyanU8 source1 = context->reg_info.id_ndsndd;
        const ZyanU8 source2 = context->reg_info.id_rm;

        if ((dest == source1) || (is_reg && (dest == source2)))
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
    }

    // If any pair of the source or destination registers are the same, the instruction results a
    // UD fault
    if (no_source_source_match) // TODO: Find better name
    {
        ZYAN_ASSERT(instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_VEX);
        ZYAN_ASSERT(is_reg);

        const ZyanU8 dest    = context->reg_info.id_reg;
        const ZyanU8 source1 = context->reg_info.id_ndsndd;
        const ZyanU8 source2 = context->reg_info.id_rm;

        if ((dest == source1) || (dest == source2) || (source1 == source2))
        {
            return ZYDIS_STATUS_BAD_REGISTER;
        }
    }

#if !defined(ZYDIS_DISABLE_AVX512) || !defined(ZYDIS_DISABLE_KNC)
    // Check for invalid MASK registers
    switch (mask_policy)
    {
    case ZYDIS_MASK_POLICY_INVALID:
    case ZYDIS_MASK_POLICY_ALLOWED:
        // Nothing to do here
        break;
    case ZYDIS_MASK_POLICY_REQUIRED:
        if (!context->vector_unified.mask)
        {
            return ZYDIS_STATUS_INVALID_MASK;
        }
        break;
    case ZYDIS_MASK_POLICY_FORBIDDEN:
        if (context->vector_unified.mask)
        {
            return ZYDIS_STATUS_INVALID_MASK;
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }
#endif

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/**
 * Uses the decoder-tree to decode the current instruction.
 *
 * @param   state       A pointer to the `ZydisDecoderState` struct.
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisDecodeInstruction(ZydisDecoderState* state,
    ZydisDecodedInstruction* instruction)
{
    ZYAN_ASSERT(state);
    ZYAN_ASSERT(instruction);

    // Iterate through the decoder tree
    const ZydisDecoderTreeNode* node = ZydisDecoderTreeGetRootNode();
    const ZydisDecoderTreeNode* temp = ZYAN_NULL;
    ZydisDecoderTreeNodeType node_type;
    do
    {
        node_type = node->type;
        ZyanU16 index = 0;
        ZyanStatus status = 0;
        switch (node_type)
        {
        case ZYDIS_NODETYPE_INVALID:
            if (temp)
            {
                node = temp;
                temp = ZYAN_NULL;
                node_type = ZYDIS_NODETYPE_FILTER_MANDATORY_PREFIX;
                if (state->prefixes.mandatory_candidate != 0x00)
                {
                    instruction->raw.prefixes[state->prefixes.offset_mandatory].type =
                        ZYDIS_PREFIX_TYPE_IGNORED;
                }
                if (state->prefixes.mandatory_candidate == 0x66)
                {
                    if (state->prefixes.offset_osz_override ==
                        state->prefixes.offset_mandatory)
                    {
                        instruction->raw.prefixes[state->prefixes.offset_mandatory].type =
                            ZYDIS_PREFIX_TYPE_EFFECTIVE;
                    }
                    instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
                }
                continue;
            }
            return ZYDIS_STATUS_DECODING_ERROR;
        case ZYDIS_NODETYPE_FILTER_XOP:
            status = ZydisNodeHandlerXOP(instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_VEX:
            status = ZydisNodeHandlerVEX(instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_EMVEX:
            status = ZydisNodeHandlerEMVEX(instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_OPCODE:
            status = ZydisNodeHandlerOpcode(state, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_MODE:
            status = ZydisNodeHandlerMode(instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_COMPACT:
            status = ZydisNodeHandlerModeCompact(instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_MODRM_MOD:
            status = ZydisNodeHandlerModrmMod(state, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_MODRM_MOD_COMPACT:
            status = ZydisNodeHandlerModrmModCompact(state, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_MODRM_REG:
            status = ZydisNodeHandlerModrmReg(state, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_MODRM_RM:
            status = ZydisNodeHandlerModrmRm(state, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_PREFIX_GROUP1:
            index = state->prefixes.group1 ? 1 : 0;
            break;
        case ZYDIS_NODETYPE_FILTER_MANDATORY_PREFIX:
            status = ZydisNodeHandlerMandatoryPrefix(state, instruction, &index);
            temp = ZydisDecoderTreeGetChildNode(node, 0);
            // TODO: Return to this point, if index == 0 contains a value and the previous path
            // TODO: was not successful
            // TODO: Restore consumed prefix
            break;
        case ZYDIS_NODETYPE_FILTER_OPERAND_SIZE:
            status = ZydisNodeHandlerOperandSize(state, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_ADDRESS_SIZE:
            status = ZydisNodeHandlerAddressSize(instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_VECTOR_LENGTH:
            status = ZydisNodeHandlerVectorLength(state->context, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_REX_W:
            status = ZydisNodeHandlerRexW(state->context, instruction, &index);
            break;
        case ZYDIS_NODETYPE_FILTER_REX_B:
            status = ZydisNodeHandlerRexB(state->context, instruction, &index);
            break;
#ifndef ZYDIS_DISABLE_AVX512
        case ZYDIS_NODETYPE_FILTER_EVEX_B:
            status = ZydisNodeHandlerEvexB(instruction, &index);
            break;
#endif
#ifndef ZYDIS_DISABLE_KNC
        case ZYDIS_NODETYPE_FILTER_MVEX_E:
            status = ZydisNodeHandlerMvexE(instruction, &index);
            break;
#endif
        case ZYDIS_NODETYPE_FILTER_MODE_AMD:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_AMD_BRANCHES));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_KNC:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_KNC));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_MPX:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_MPX));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_CET:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_CET));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_LZCNT:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_LZCNT));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_TZCNT:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_TZCNT));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_WBNOINVD:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_WBNOINVD));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_CLDEMOTE:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_CLDEMOTE));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_IPREFETCH:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_IPREFETCH));
            break;
        case ZYDIS_NODETYPE_FILTER_MODE_UD0_COMPAT:
            index = !!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_UD0_COMPAT));
            break;
        default:
            if (node_type & ZYDIS_NODETYPE_DEFINITION_MASK)
            {
                const ZydisInstructionDefinition* definition;
                ZydisGetInstructionDefinition(instruction->encoding, node->value, &definition);
                ZydisSetEffectiveOperandWidth(state->context, instruction, definition);
                ZydisSetEffectiveAddressWidth(state->context, instruction, definition);

                const ZydisInstructionEncodingInfo* info;
                ZydisGetInstructionEncodingInfo(node, &info);
                ZYAN_CHECK(ZydisDecodeOptionalInstructionParts(state, instruction, info));
                ZYAN_CHECK(ZydisCheckErrorConditions(state, instruction, definition));

                if (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_3DNOW)
                {
                    // Get actual 3DNOW opcode and definition
                    ZYAN_CHECK(ZydisInputNext(state, instruction, &instruction->opcode));
                    node = ZydisDecoderTreeGetRootNode();
                    node = ZydisDecoderTreeGetChildNode(node, 0x0F);
                    node = ZydisDecoderTreeGetChildNode(node, 0x0F);
                    node = ZydisDecoderTreeGetChildNode(node, instruction->opcode);
                    if (node->type == ZYDIS_NODETYPE_INVALID)
                    {
                        return ZYDIS_STATUS_DECODING_ERROR;
                    }
                    ZYAN_ASSERT(node->type == ZYDIS_NODETYPE_FILTER_MODRM_MOD_COMPACT);
                    node = ZydisDecoderTreeGetChildNode(
                        node, (instruction->raw.modrm.mod == 0x3) ? 0 : 1);
                    ZYAN_ASSERT(node->type & ZYDIS_NODETYPE_DEFINITION_MASK);
                    ZydisGetInstructionDefinition(instruction->encoding, node->value, &definition);
                }

                instruction->mnemonic = definition->mnemonic;

#ifndef ZYDIS_MINIMAL_MODE

                instruction->operand_count = definition->operand_count;
                instruction->operand_count_visible = definition->operand_count_visible;
                state->context->definition = definition;

                instruction->meta.category = definition->category;
                instruction->meta.isa_set = definition->isa_set;
                instruction->meta.isa_ext = definition->isa_ext;
                instruction->meta.branch_type = definition->branch_type;
                ZYAN_ASSERT((instruction->meta.branch_type == ZYDIS_BRANCH_TYPE_NONE) ||
                        ((instruction->meta.category == ZYDIS_CATEGORY_CALL) ||
                         (instruction->meta.category == ZYDIS_CATEGORY_COND_BR) ||
                         (instruction->meta.category == ZYDIS_CATEGORY_UNCOND_BR) ||
                         (instruction->meta.category == ZYDIS_CATEGORY_RET)));
                instruction->meta.exception_class = definition->exception_class;

                if (!(state->decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_MINIMAL)))
                {
                    ZydisSetAttributes(state, instruction, definition);
                    switch (instruction->encoding)
                    {
                    case ZYDIS_INSTRUCTION_ENCODING_XOP:
                    case ZYDIS_INSTRUCTION_ENCODING_VEX:
                    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
                    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
                        ZydisSetAVXInformation(state->context, instruction, definition);
                        break;
                    default:
                        break;
                    }

                    const ZydisDefinitionAccessedFlags* flags;
                    if (ZydisGetAccessedFlags(definition, &flags))
                    {
                        instruction->attributes |= ZYDIS_ATTRIB_CPUFLAG_ACCESS;
                    }
                    instruction->cpu_flags = &flags->cpu_flags;
                    instruction->fpu_flags = &flags->fpu_flags;
                }

#endif

                return ZYAN_STATUS_SUCCESS;
            }
            ZYAN_UNREACHABLE;
        }
        ZYAN_CHECK(status);
        node = ZydisDecoderTreeGetChildNode(node, index);
    } while ((node_type != ZYDIS_NODETYPE_INVALID) && !(node_type & ZYDIS_NODETYPE_DEFINITION_MASK));
    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

ZyanStatus ZydisDecoderInit(ZydisDecoder* decoder, ZydisMachineMode machine_mode,
    ZydisStackWidth stack_width)
{
    ZYAN_STATIC_ASSERT(ZYDIS_DECODER_MODE_MAX_VALUE <= 32);

    static const ZyanU32 decoder_modes =
#ifdef ZYDIS_MINIMAL_MODE
        (1 << ZYDIS_DECODER_MODE_MINIMAL) |
#endif
        (1 << ZYDIS_DECODER_MODE_MPX) |
        (1 << ZYDIS_DECODER_MODE_CET) |
        (1 << ZYDIS_DECODER_MODE_LZCNT) |
        (1 << ZYDIS_DECODER_MODE_TZCNT) |
        (1 << ZYDIS_DECODER_MODE_CLDEMOTE) |
        (1 << ZYDIS_DECODER_MODE_IPREFETCH);

    if (!decoder)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    switch (machine_mode)
    {
    case ZYDIS_MACHINE_MODE_LONG_64:
        if (stack_width != ZYDIS_STACK_WIDTH_64)
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
    case ZYDIS_MACHINE_MODE_LEGACY_32:
    case ZYDIS_MACHINE_MODE_LEGACY_16:
    case ZYDIS_MACHINE_MODE_REAL_16:
        if ((stack_width != ZYDIS_STACK_WIDTH_16) && (stack_width != ZYDIS_STACK_WIDTH_32))
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    decoder->machine_mode = machine_mode;
    decoder->stack_width = stack_width;
    decoder->decoder_mode = decoder_modes;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisDecoderEnableMode(ZydisDecoder* decoder, ZydisDecoderMode mode, ZyanBool enabled)
{
    if (!decoder || ((ZyanUSize)mode > ZYDIS_DECODER_MODE_MAX_VALUE))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

#ifdef ZYDIS_MINIMAL_MODE
    if ((mode == ZYDIS_DECODER_MODE_MINIMAL) && !enabled)
    {
        return ZYAN_STATUS_INVALID_OPERATION;
    }
#endif

    if (enabled)
    {
        decoder->decoder_mode |= (1 << mode);
    }
    else
    {
        decoder->decoder_mode &= ~(1 << mode);
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisDecoderDecodeFull(const ZydisDecoder* decoder,
    const void* buffer, ZyanUSize length, ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT])
{
    if (!decoder || !instruction || !buffer || !operands)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (!length)
    {
        return ZYDIS_STATUS_NO_MORE_DATA;
    }
    if (decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_MINIMAL))
    {
        return ZYAN_STATUS_MISSING_DEPENDENCY; // TODO: Introduce better status code
    }

    ZydisDecoderContext context;
    ZYAN_CHECK(ZydisDecoderDecodeInstruction(decoder, &context, buffer, length, instruction));
    ZYAN_CHECK(ZydisDecoderDecodeOperands(decoder, &context, instruction, operands,
        instruction->operand_count));
    ZYAN_MEMSET(&operands[instruction->operand_count], 0,
        (ZYDIS_MAX_OPERAND_COUNT - instruction->operand_count) * sizeof(operands[0]));

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisDecoderDecodeInstruction(const ZydisDecoder* decoder, ZydisDecoderContext* context,
    const void* buffer, ZyanUSize length, ZydisDecodedInstruction* instruction)
{
    if (!decoder || !instruction || !buffer)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (!length)
    {
        return ZYDIS_STATUS_NO_MORE_DATA;
    }

    ZydisDecoderState state;
    ZYAN_MEMSET(&state, 0, sizeof(state));
    state.decoder = decoder;
    state.buffer = (const ZyanU8*)buffer;
    state.buffer_len = length;
    state.prefixes.offset_notrack = -1;

    ZydisDecoderContext default_context;
    if (!context)
    {
        // Use a fallback context if no custom one has been provided
        context = &default_context;
    }
    ZYAN_MEMSET(context, 0, sizeof(*context));
    state.context = context;

    ZYAN_MEMSET(instruction, 0, sizeof(*instruction));
    instruction->machine_mode = decoder->machine_mode;
    instruction->stack_width = 16 << decoder->stack_width;

    ZYAN_CHECK(ZydisCollectOptionalPrefixes(&state, instruction));
    ZYAN_CHECK(ZydisDecodeInstruction(&state, instruction));

    instruction->raw.encoding2 = instruction->encoding;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisDecoderDecodeOperands(const ZydisDecoder* decoder,
    const ZydisDecoderContext* context, const ZydisDecodedInstruction* instruction,
    ZydisDecodedOperand* operands, ZyanU8 operand_count)
{
#ifdef ZYDIS_MINIMAL_MODE

    ZYAN_UNUSED(decoder);
    ZYAN_UNUSED(context);
    ZYAN_UNUSED(instruction);
    ZYAN_UNUSED(operands);
    ZYAN_UNUSED(operand_count);

    return ZYAN_STATUS_MISSING_DEPENDENCY; // TODO: Introduce better status code

#else

    if (!decoder || !context || !context->definition || !instruction ||
        (operand_count && !operands) || (operand_count > ZYDIS_MAX_OPERAND_COUNT))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (decoder->decoder_mode & (1 << ZYDIS_DECODER_MODE_MINIMAL))
    {
        return ZYAN_STATUS_MISSING_DEPENDENCY; // TODO: Introduce better status code
    }

    operand_count = ZYAN_MIN(operand_count, instruction->operand_count);
    if (!operand_count)
    {
        return ZYAN_STATUS_SUCCESS;
    }

    return ZydisDecodeOperands(decoder, context, instruction, operands, operand_count);

#endif
}

/* ============================================================================================== */
