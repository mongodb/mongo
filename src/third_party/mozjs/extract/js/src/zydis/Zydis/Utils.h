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
 * @brief   Other utility functions.
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
 * @brief   Defines the `ZydisInstructionSegment` struct.
 */
typedef enum ZydisInstructionSegment_
{
    ZYDIS_INSTR_SEGMENT_NONE,
    /**
     * @brief   The legacy prefixes (including ignored `REX` prefixes).
     */
    ZYDIS_INSTR_SEGMENT_PREFIXES,
    /**
     * @brief   The effective `REX` prefix byte.
     */
    ZYDIS_INSTR_SEGMENT_REX,
    /**
     * @brief   The `XOP` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_XOP,
    /**
     * @brief   The `VEX` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_VEX,
    /**
     * @brief   The `EVEX` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_EVEX,
    /**
     * @brief   The `MVEX` prefix bytes.
     */
    ZYDIS_INSTR_SEGMENT_MVEX,
    /**
     * @brief   The opcode bytes.
     */
    ZYDIS_INSTR_SEGMENT_OPCODE,
    /**
     * @brief   The `ModRM` byte.
     */
    ZYDIS_INSTR_SEGMENT_MODRM,
    /**
     * @brief   The `SIB` byte.
     */
    ZYDIS_INSTR_SEGMENT_SIB,
    /**
     * @brief   The displacement bytes.
     */
    ZYDIS_INSTR_SEGMENT_DISPLACEMENT,
    /**
     * @brief   The immediate bytes.
     */
    ZYDIS_INSTR_SEGMENT_IMMEDIATE,

    /**
     * @brief   Maximum value of this enum.
     */
    ZYDIS_INSTR_SEGMENT_MAX_VALUE = ZYDIS_INSTR_SEGMENT_IMMEDIATE,
    /**
     * @brief   The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_INSTR_SEGMENT_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_INSTR_SEGMENT_MAX_VALUE)
} ZydisInstructionSegment;

/**
 * @brief   Defines the `ZydisInstructionSegments` struct.
 */
typedef struct ZydisInstructionSegments_
{
    /**
     * @brief   The number of logical instruction segments.
     */
    ZyanU8 count;
    struct
    {
        /**
         * @brief   The type of the segment.
         */
        ZydisInstructionSegment type;
        /**
         * @brief   The offset of the segment relative to the start of the instruction (in bytes).
         */
        ZyanU8 offset;
        /**
         * @brief   The size of the segment, in bytes.
         */
        ZyanU8 size;
    } segments[ZYDIS_MAX_INSTRUCTION_SEGMENT_COUNT];
} ZydisInstructionSegments;

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup utils Utils
 * @brief Miscellaneous utility functions. Address translation and other helpers.
 * @{
 */

/* ---------------------------------------------------------------------------------------------- */
/* Address calculation                                                                            */
/* ---------------------------------------------------------------------------------------------- */

// TODO: Provide a function that works in minimal-mode and does not require a operand parameter

/**
 * @brief   Calculates the absolute address value for the given instruction operand.
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
 * @brief   Calculates the absolute address value for the given instruction operand.
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
/* Accessed CPU flags                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns a mask of accessed CPU-flags matching the given `action`.
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   action      The CPU-flag action.
 * @param   flags       Receives the flag mask.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisGetAccessedFlagsByAction(const ZydisDecodedInstruction* instruction,
    ZydisCPUFlagAction action, ZydisCPUFlags* flags);

/**
 * @brief   Returns a mask of accessed CPU-flags that are read (tested) by the current instruction.
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   flags       Receives the flag mask.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisGetAccessedFlagsRead(const ZydisDecodedInstruction* instruction,
    ZydisCPUFlags* flags);

/**
 * @brief   Returns a mask of accessed CPU-flags that are written (modified, undefined) by the
 *          current instruction.
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   flags       Receives the flag mask.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisGetAccessedFlagsWritten(const ZydisDecodedInstruction* instruction,
    ZydisCPUFlags* flags);

/* ---------------------------------------------------------------------------------------------- */
/* Instruction segments                                                                           */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns offsets and sizes of all logical instruction segments (e.g. `OPCODE`,
 *          `MODRM`, ...).
 *
 * @param   instruction A pointer to the `ZydisDecodedInstruction` struct.
 * @param   segments    Receives the instruction segments information.
 *
 * @return  A zyan status code.
 */
ZYDIS_EXPORT ZyanStatus ZydisGetInstructionSegments(const ZydisDecodedInstruction* instruction,
    ZydisInstructionSegments* segments);

/* ---------------------------------------------------------------------------------------------- */

/**
 * @}
 */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_UTILS_H */
