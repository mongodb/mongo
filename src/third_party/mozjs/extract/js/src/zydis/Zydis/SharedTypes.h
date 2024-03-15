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
 * Defines decoder/encoder-shared macros and types.
 */

#ifndef ZYDIS_SHAREDTYPES_H
#define ZYDIS_SHAREDTYPES_H

#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constants                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_MAX_INSTRUCTION_LENGTH    15
#define ZYDIS_MAX_OPERAND_COUNT         10 // TODO: Auto generate
#define ZYDIS_MAX_OPERAND_COUNT_VISIBLE  5 // TODO: Auto generate

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Machine mode                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisMachineMode` enum.
 */
typedef enum ZydisMachineMode_
{
    /**
     * 64 bit mode.
     */
    ZYDIS_MACHINE_MODE_LONG_64,
    /**
     * 32 bit protected mode.
     */
    ZYDIS_MACHINE_MODE_LONG_COMPAT_32,
    /**
     * 16 bit protected mode.
     */
    ZYDIS_MACHINE_MODE_LONG_COMPAT_16,
    /**
     * 32 bit protected mode.
     */
    ZYDIS_MACHINE_MODE_LEGACY_32,
    /**
     * 16 bit protected mode.
     */
    ZYDIS_MACHINE_MODE_LEGACY_16,
    /**
     * 16 bit real mode.
     */
    ZYDIS_MACHINE_MODE_REAL_16,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_MACHINE_MODE_MAX_VALUE = ZYDIS_MACHINE_MODE_REAL_16,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_MACHINE_MODE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_MACHINE_MODE_MAX_VALUE)
} ZydisMachineMode;

/* ---------------------------------------------------------------------------------------------- */
/* Stack width                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisStackWidth` enum.
 */
typedef enum ZydisStackWidth_
{
    ZYDIS_STACK_WIDTH_16,
    ZYDIS_STACK_WIDTH_32,
    ZYDIS_STACK_WIDTH_64,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_STACK_WIDTH_MAX_VALUE = ZYDIS_STACK_WIDTH_64,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_STACK_WIDTH_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_STACK_WIDTH_MAX_VALUE)
} ZydisStackWidth;

/* ---------------------------------------------------------------------------------------------- */
/* Element type                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisElementType` enum.
 */
typedef enum ZydisElementType_
{
    ZYDIS_ELEMENT_TYPE_INVALID,
    /**
     * A struct type.
     */
    ZYDIS_ELEMENT_TYPE_STRUCT,
    /**
     * Unsigned integer value.
     */
    ZYDIS_ELEMENT_TYPE_UINT,
    /**
     * Signed integer value.
     */
    ZYDIS_ELEMENT_TYPE_INT,
    /**
     * 16-bit floating point value (`half`).
     */
    ZYDIS_ELEMENT_TYPE_FLOAT16,
    /**
     * 32-bit floating point value (`single`).
     */
    ZYDIS_ELEMENT_TYPE_FLOAT32,
    /**
     * 64-bit floating point value (`double`).
     */
    ZYDIS_ELEMENT_TYPE_FLOAT64,
    /**
     * 80-bit floating point value (`extended`).
     */
    ZYDIS_ELEMENT_TYPE_FLOAT80,
    /**
     * Binary coded decimal value.
     */
    ZYDIS_ELEMENT_TYPE_LONGBCD,
    /**
     * A condition code (e.g. used by `CMPPD`, `VCMPPD`, ...).
     */
    ZYDIS_ELEMENT_TYPE_CC,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_ELEMENT_TYPE_MAX_VALUE = ZYDIS_ELEMENT_TYPE_CC,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_ELEMENT_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_ELEMENT_TYPE_MAX_VALUE)
} ZydisElementType;

/* ---------------------------------------------------------------------------------------------- */
/* Element size                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisElementSize` datatype.
 */
typedef ZyanU16 ZydisElementSize;

/* ---------------------------------------------------------------------------------------------- */
/* Operand type                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisOperandType` enum.
 */
typedef enum ZydisOperandType_
{
    /**
     * The operand is not used.
     */
    ZYDIS_OPERAND_TYPE_UNUSED,
    /**
     * The operand is a register operand.
     */
    ZYDIS_OPERAND_TYPE_REGISTER,
    /**
     * The operand is a memory operand.
     */
    ZYDIS_OPERAND_TYPE_MEMORY,
    /**
     * The operand is a pointer operand with a segment:offset lvalue.
     */
    ZYDIS_OPERAND_TYPE_POINTER,
    /**
     * The operand is an immediate operand.
     */
    ZYDIS_OPERAND_TYPE_IMMEDIATE,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_OPERAND_TYPE_MAX_VALUE = ZYDIS_OPERAND_TYPE_IMMEDIATE,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_OPERAND_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_OPERAND_TYPE_MAX_VALUE)
} ZydisOperandType;

// If asserts are failing here remember to update encoder table generator before fixing asserts
ZYAN_STATIC_ASSERT(ZYAN_BITS_TO_REPRESENT(
    ZYDIS_OPERAND_TYPE_MAX_VALUE - ZYDIS_OPERAND_TYPE_REGISTER) == 2);

/* ---------------------------------------------------------------------------------------------- */
/* Operand encoding                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisOperandEncoding` enum.
 */
typedef enum ZydisOperandEncoding_
{
    ZYDIS_OPERAND_ENCODING_NONE,
    ZYDIS_OPERAND_ENCODING_MODRM_REG,
    ZYDIS_OPERAND_ENCODING_MODRM_RM,
    ZYDIS_OPERAND_ENCODING_OPCODE,
    ZYDIS_OPERAND_ENCODING_NDSNDD,
    ZYDIS_OPERAND_ENCODING_IS4,
    ZYDIS_OPERAND_ENCODING_MASK,
    ZYDIS_OPERAND_ENCODING_DISP8,
    ZYDIS_OPERAND_ENCODING_DISP16,
    ZYDIS_OPERAND_ENCODING_DISP32,
    ZYDIS_OPERAND_ENCODING_DISP64,
    ZYDIS_OPERAND_ENCODING_DISP16_32_64,
    ZYDIS_OPERAND_ENCODING_DISP32_32_64,
    ZYDIS_OPERAND_ENCODING_DISP16_32_32,
    ZYDIS_OPERAND_ENCODING_UIMM8,
    ZYDIS_OPERAND_ENCODING_UIMM16,
    ZYDIS_OPERAND_ENCODING_UIMM32,
    ZYDIS_OPERAND_ENCODING_UIMM64,
    ZYDIS_OPERAND_ENCODING_UIMM16_32_64,
    ZYDIS_OPERAND_ENCODING_UIMM32_32_64,
    ZYDIS_OPERAND_ENCODING_UIMM16_32_32,
    ZYDIS_OPERAND_ENCODING_SIMM8,
    ZYDIS_OPERAND_ENCODING_SIMM16,
    ZYDIS_OPERAND_ENCODING_SIMM32,
    ZYDIS_OPERAND_ENCODING_SIMM64,
    ZYDIS_OPERAND_ENCODING_SIMM16_32_64,
    ZYDIS_OPERAND_ENCODING_SIMM32_32_64,
    ZYDIS_OPERAND_ENCODING_SIMM16_32_32,
    ZYDIS_OPERAND_ENCODING_JIMM8,
    ZYDIS_OPERAND_ENCODING_JIMM16,
    ZYDIS_OPERAND_ENCODING_JIMM32,
    ZYDIS_OPERAND_ENCODING_JIMM64,
    ZYDIS_OPERAND_ENCODING_JIMM16_32_64,
    ZYDIS_OPERAND_ENCODING_JIMM32_32_64,
    ZYDIS_OPERAND_ENCODING_JIMM16_32_32,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_OPERAND_ENCODING_MAX_VALUE = ZYDIS_OPERAND_ENCODING_JIMM16_32_32,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_OPERAND_ENCODING_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_OPERAND_ENCODING_MAX_VALUE)
} ZydisOperandEncoding;

/* ---------------------------------------------------------------------------------------------- */
/* Operand visibility                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisOperandVisibility` enum.
 */
typedef enum ZydisOperandVisibility_
{
    ZYDIS_OPERAND_VISIBILITY_INVALID,
    /**
     * The operand is explicitly encoded in the instruction.
     */
    ZYDIS_OPERAND_VISIBILITY_EXPLICIT,
    /**
     * The operand is part of the opcode, but listed as an operand.
     */
    ZYDIS_OPERAND_VISIBILITY_IMPLICIT,
    /**
     * The operand is part of the opcode, and not typically listed as an operand.
     */
    ZYDIS_OPERAND_VISIBILITY_HIDDEN,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_OPERAND_VISIBILITY_MAX_VALUE = ZYDIS_OPERAND_VISIBILITY_HIDDEN,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_OPERAND_VISIBILITY_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_OPERAND_VISIBILITY_MAX_VALUE)
} ZydisOperandVisibility;

/* ---------------------------------------------------------------------------------------------- */
/* Operand action                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisOperandAction` enum.
 */
typedef enum ZydisOperandAction_
{
    /* ------------------------------------------------------------------------------------------ */
    /* Elemental actions                                                                          */
    /* ------------------------------------------------------------------------------------------ */

    /**
     * The operand is read by the instruction.
     */
    ZYDIS_OPERAND_ACTION_READ       = 0x01,
    /**
     * The operand is written by the instruction (must write).
     */
    ZYDIS_OPERAND_ACTION_WRITE      = 0x02,
    /**
     * The operand is conditionally read by the instruction.
     */
    ZYDIS_OPERAND_ACTION_CONDREAD   = 0x04,
    /**
     * The operand is conditionally written by the instruction (may write).
     */
    ZYDIS_OPERAND_ACTION_CONDWRITE  = 0x08,

    /* ------------------------------------------------------------------------------------------ */
    /* Combined actions                                                                           */
    /* ------------------------------------------------------------------------------------------ */

    /**
     * The operand is read (must read) and written by the instruction (must write).
     */
    ZYDIS_OPERAND_ACTION_READWRITE = ZYDIS_OPERAND_ACTION_READ | ZYDIS_OPERAND_ACTION_WRITE,
    /**
     * The operand is conditionally read (may read) and conditionally written by
     * the instruction (may write).
     */
    ZYDIS_OPERAND_ACTION_CONDREAD_CONDWRITE =
        ZYDIS_OPERAND_ACTION_CONDREAD | ZYDIS_OPERAND_ACTION_CONDWRITE,
    /**
     * The operand is read (must read) and conditionally written by the
     * instruction (may write).
     */
    ZYDIS_OPERAND_ACTION_READ_CONDWRITE =
        ZYDIS_OPERAND_ACTION_READ | ZYDIS_OPERAND_ACTION_CONDWRITE,
    /**
     * The operand is written (must write) and conditionally read by the
     * instruction (may read).
     */
    ZYDIS_OPERAND_ACTION_CONDREAD_WRITE =
        ZYDIS_OPERAND_ACTION_CONDREAD | ZYDIS_OPERAND_ACTION_WRITE,

    /**
     * Mask combining all reading access flags.
     */
    ZYDIS_OPERAND_ACTION_MASK_READ  = ZYDIS_OPERAND_ACTION_READ | ZYDIS_OPERAND_ACTION_CONDREAD,
    /**
     * Mask combining all writing access flags.
     */
    ZYDIS_OPERAND_ACTION_MASK_WRITE = ZYDIS_OPERAND_ACTION_WRITE | ZYDIS_OPERAND_ACTION_CONDWRITE,

    /* ------------------------------------------------------------------------------------------ */

    /**
     * The minimum number of bits required to represent all values of this bitset.
     */
    ZYDIS_OPERAND_ACTION_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_OPERAND_ACTION_CONDWRITE)
} ZydisOperandAction;

/**
 * Defines the `ZydisOperandActions` data-type.
 */
typedef ZyanU8 ZydisOperandActions;

/* ---------------------------------------------------------------------------------------------- */
/* Instruction encoding                                                                           */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisInstructionEncoding` enum.
 */
typedef enum ZydisInstructionEncoding_
{
    /**
     * The instruction uses the legacy encoding.
     */
    ZYDIS_INSTRUCTION_ENCODING_LEGACY,
    /**
     * The instruction uses the AMD 3DNow-encoding.
     */
    ZYDIS_INSTRUCTION_ENCODING_3DNOW,
    /**
     * The instruction uses the AMD XOP-encoding.
     */
    ZYDIS_INSTRUCTION_ENCODING_XOP,
    /**
     * The instruction uses the VEX-encoding.
     */
    ZYDIS_INSTRUCTION_ENCODING_VEX,
    /**
     * The instruction uses the EVEX-encoding.
     */
    ZYDIS_INSTRUCTION_ENCODING_EVEX,
    /**
     * The instruction uses the MVEX-encoding.
     */
    ZYDIS_INSTRUCTION_ENCODING_MVEX,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_INSTRUCTION_ENCODING_MAX_VALUE = ZYDIS_INSTRUCTION_ENCODING_MVEX,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_INSTRUCTION_ENCODING_REQUIRED_BITS =
        ZYAN_BITS_TO_REPRESENT(ZYDIS_INSTRUCTION_ENCODING_MAX_VALUE)
} ZydisInstructionEncoding;

/* ---------------------------------------------------------------------------------------------- */
/* Opcode map                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines the `ZydisOpcodeMap` enum.
 */
typedef enum ZydisOpcodeMap_
{
    ZYDIS_OPCODE_MAP_DEFAULT,
    ZYDIS_OPCODE_MAP_0F,
    ZYDIS_OPCODE_MAP_0F38,
    ZYDIS_OPCODE_MAP_0F3A,
    ZYDIS_OPCODE_MAP_MAP4, // not used
    ZYDIS_OPCODE_MAP_MAP5,
    ZYDIS_OPCODE_MAP_MAP6,
    ZYDIS_OPCODE_MAP_MAP7, // not used
    ZYDIS_OPCODE_MAP_0F0F,
    ZYDIS_OPCODE_MAP_XOP8,
    ZYDIS_OPCODE_MAP_XOP9,
    ZYDIS_OPCODE_MAP_XOPA,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_OPCODE_MAP_MAX_VALUE = ZYDIS_OPCODE_MAP_XOPA,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_OPCODE_MAP_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_OPCODE_MAP_MAX_VALUE)
} ZydisOpcodeMap;

/* ---------------------------------------------------------------------------------------------- */
/* Instruction attributes                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @defgroup instruction_attributes Instruction attributes
 *
 * Constants describing various properties of an instruction. Used in the 
 * @ref ZydisDecodedInstruction.attributes and @ref ZydisEncoderRequest.prefixes fields.
 *
 * @{
 */

/**
 * Defines the `ZydisInstructionAttributes` data-type.
 */
typedef ZyanU64 ZydisInstructionAttributes;

/**
 * The instruction has the `ModRM` byte.
 */
#define ZYDIS_ATTRIB_HAS_MODRM                  (1ULL <<  0)
/**
 * The instruction has the `SIB` byte.
 */
#define ZYDIS_ATTRIB_HAS_SIB                    (1ULL <<  1)
/**
 * The instruction has the `REX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_REX                    (1ULL <<  2)
/**
 * The instruction has the `XOP` prefix.
 */
#define ZYDIS_ATTRIB_HAS_XOP                    (1ULL <<  3)
/**
 * The instruction has the `VEX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_VEX                    (1ULL <<  4)
/**
 * The instruction has the `EVEX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_EVEX                   (1ULL <<  5)
/**
 * The instruction has the `MVEX` prefix.
 */
#define ZYDIS_ATTRIB_HAS_MVEX                   (1ULL <<  6)
/**
 * The instruction has one or more operands with position-relative offsets.
 */
#define ZYDIS_ATTRIB_IS_RELATIVE                (1ULL <<  7)
/**
 * The instruction is privileged.
 *
 * Privileged instructions are any instructions that require a current ring level below 3.
 */
#define ZYDIS_ATTRIB_IS_PRIVILEGED              (1ULL <<  8)
/**
 * The instruction accesses one or more CPU-flags.
 */
#define ZYDIS_ATTRIB_CPUFLAG_ACCESS             (1ULL <<  9)
/**
 * The instruction may conditionally read the general CPU state.
 */
#define ZYDIS_ATTRIB_CPU_STATE_CR               (1ULL << 10)
/**
 * The instruction may conditionally write the general CPU state.
 */
#define ZYDIS_ATTRIB_CPU_STATE_CW               (1ULL << 11)
/**
 * The instruction may conditionally read the FPU state (X87, MMX).
 */
#define ZYDIS_ATTRIB_FPU_STATE_CR               (1ULL << 12)
/**
 * The instruction may conditionally write the FPU state (X87, MMX).
 */
#define ZYDIS_ATTRIB_FPU_STATE_CW               (1ULL << 13)
/**
 * The instruction may conditionally read the XMM state (AVX, AVX2, AVX-512).
 */
#define ZYDIS_ATTRIB_XMM_STATE_CR               (1ULL << 14)
/**
 * The instruction may conditionally write the XMM state (AVX, AVX2, AVX-512).
 */
#define ZYDIS_ATTRIB_XMM_STATE_CW               (1ULL << 15)
/**
 * The instruction accepts the `LOCK` prefix (`0xF0`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_LOCK               (1ULL << 16)
/**
 * The instruction accepts the `REP` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REP                (1ULL << 17)
/**
 * The instruction accepts the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPE               (1ULL << 18)
/**
 * The instruction accepts the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPZ               ZYDIS_ATTRIB_ACCEPTS_REPE
/**
 * The instruction accepts the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPNE              (1ULL << 19)
/**
 * The instruction accepts the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_REPNZ              ZYDIS_ATTRIB_ACCEPTS_REPNE
/**
 * The instruction accepts the `BND` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_BND                (1ULL << 20)
/**
 * The instruction accepts the `XACQUIRE` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_XACQUIRE           (1ULL << 21)
/**
 * The instruction accepts the `XRELEASE` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_XRELEASE           (1ULL << 22)
/**
 * The instruction accepts the `XACQUIRE`/`XRELEASE` prefixes (`0xF2`, `0xF3`)
 * without the `LOCK` prefix (`0x0F`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_HLE_WITHOUT_LOCK   (1ULL << 23)
/**
 * The instruction accepts branch hints (0x2E, 0x3E).
 */
#define ZYDIS_ATTRIB_ACCEPTS_BRANCH_HINTS       (1ULL << 24)
/**
 * The instruction accepts the `CET` `no-track` prefix (`0x3E`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_NOTRACK            (1ULL << 25)
/**
 * The instruction accepts segment prefixes (`0x2E`, `0x36`, `0x3E`, `0x26`,
 * `0x64`, `0x65`).
 */
#define ZYDIS_ATTRIB_ACCEPTS_SEGMENT            (1ULL << 26)
/**
 * The instruction has the `LOCK` prefix (`0xF0`).
 */
#define ZYDIS_ATTRIB_HAS_LOCK                   (1ULL << 27)
/**
 * The instruction has the `REP` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_REP                    (1ULL << 28)
/**
 * The instruction has the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_REPE                   (1ULL << 29)
/**
 * The instruction has the `REPE`/`REPZ` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_REPZ                   ZYDIS_ATTRIB_HAS_REPE
/**
 * The instruction has the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_REPNE                  (1ULL << 30)
/**
 * The instruction has the `REPNE`/`REPNZ` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_REPNZ                  ZYDIS_ATTRIB_HAS_REPNE
/**
 * The instruction has the `BND` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_BND                    (1ULL << 31)
/**
 * The instruction has the `XACQUIRE` prefix (`0xF2`).
 */
#define ZYDIS_ATTRIB_HAS_XACQUIRE               (1ULL << 32)
/**
 * The instruction has the `XRELEASE` prefix (`0xF3`).
 */
#define ZYDIS_ATTRIB_HAS_XRELEASE               (1ULL << 33)
/**
 * The instruction has the branch-not-taken hint (`0x2E`).
 */
#define ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN       (1ULL << 34)
/**
 * The instruction has the branch-taken hint (`0x3E`).
 */
#define ZYDIS_ATTRIB_HAS_BRANCH_TAKEN           (1ULL << 35)
/**
 * The instruction has the `CET` `no-track` prefix (`0x3E`).
 */
#define ZYDIS_ATTRIB_HAS_NOTRACK                (1ULL << 36)
/**
 * The instruction has the `CS` segment modifier (`0x2E`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_CS             (1ULL << 37)
/**
 * The instruction has the `SS` segment modifier (`0x36`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_SS             (1ULL << 38)
/**
 * The instruction has the `DS` segment modifier (`0x3E`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_DS             (1ULL << 39)
/**
 * The instruction has the `ES` segment modifier (`0x26`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_ES             (1ULL << 40)
/**
 * The instruction has the `FS` segment modifier (`0x64`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_FS             (1ULL << 41)
/**
 * The instruction has the `GS` segment modifier (`0x65`).
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT_GS             (1ULL << 42)
/**
 * The instruction has a segment modifier.
 */
#define ZYDIS_ATTRIB_HAS_SEGMENT                (ZYDIS_ATTRIB_HAS_SEGMENT_CS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_SS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_DS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_ES | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_FS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_GS)
/**
 * The instruction has the operand-size override prefix (`0x66`).
 */
#define ZYDIS_ATTRIB_HAS_OPERANDSIZE            (1ULL << 43) // TODO: rename
/**
 * The instruction has the address-size override prefix (`0x67`).
 */
#define ZYDIS_ATTRIB_HAS_ADDRESSSIZE            (1ULL << 44) // TODO: rename
/**
 * The instruction has the `EVEX.b` bit set.
 *
 * This attribute is mainly used by the encoder.
 */
#define ZYDIS_ATTRIB_HAS_EVEX_B                 (1ULL << 45) // TODO: rename

/**
 * @}
 */

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_SHAREDTYPES_H */
