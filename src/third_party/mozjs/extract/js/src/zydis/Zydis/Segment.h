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
 * Functions and types providing encoding information about individual instruction bytes.
 */

#ifndef ZYDIS_SEGMENT_H
#define ZYDIS_SEGMENT_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zydis/DecoderTypes.h"
#include "zydis/Zydis/Status.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
* @addtogroup segment Segment
* Functions and types providing encoding information about individual instruction bytes.
* @{
*/

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constants                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_MAX_INSTRUCTION_SEGMENT_COUNT 9

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZydisInstructionSegment` struct.
 */
typedef enum ZydisInstructionSegment_
{
    ZYDIS_INSTR_SEGMENT_NONE,
    /**
     * The legacy prefixes (including ignored `REX` prefixes).
     */
    ZYDIS_INSTR_SEGMENT_PREFIXES,
    /**
     * The effective `REX` prefix byte.
     */
    ZYDIS_INSTR_SEGMENT_REX,
    /**
     * The `XOP` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_XOP,
    /**
     * The `VEX` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_VEX,
    /**
     * The `EVEX` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_EVEX,
    /**
     * The `MVEX` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_MVEX,
    /**
     * The opcode bytes.
     */
    ZYDIS_INSTR_SEGMENT_OPCODE,
    /**
     * The `ModRM` byte.
     */
    ZYDIS_INSTR_SEGMENT_MODRM,
    /**
     * The `SIB` byte.
     */
    ZYDIS_INSTR_SEGMENT_SIB,
    /**
     * The displacement bytes.
     */
    ZYDIS_INSTR_SEGMENT_DISPLACEMENT,
    /**
     * The immediate bytes.
     */
    ZYDIS_INSTR_SEGMENT_IMMEDIATE,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_INSTR_SEGMENT_MAX_VALUE = ZYDIS_INSTR_SEGMENT_IMMEDIATE,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_INSTR_SEGMENT_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_INSTR_SEGMENT_MAX_VALUE)
} ZydisInstructionSegment;

/**
 * Defines the `ZydisInstructionSegments` struct.
 */
typedef struct ZydisInstructionSegments_
{
    /**
     * The number of logical instruction segments.
     */
    ZyanU8 count;
    struct
    {
        /**
         * The type of the segment.
         */
        ZydisInstructionSegment type;
        /**
         * The offset of the segment relative to the start of the instruction (in bytes).
         */
        ZyanU8 offset;
        /**
         * The size of the segment, in bytes.
         */
        ZyanU8 size;
    } segments[ZYDIS_MAX_INSTRUCTION_SEGMENT_COUNT];
} ZydisInstructionSegments;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * Returns offsets and sizes of all logical instruction segments (e.g. `OPCODE`,
 * `MODRM`, ...).
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   segments    Receives the instruction segments information.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisGetInstructionSegments(const ZydisDecodedInstruction* instruction,
        ZydisInstructionSegments* segments);

/* ============================================================================================== */

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_SEGMENT_H */
