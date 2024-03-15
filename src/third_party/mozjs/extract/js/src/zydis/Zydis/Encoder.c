/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Mappa

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

// ReSharper disable CppClangTidyClangDiagnosticSwitchEnum
// ReSharper disable CppClangTidyClangDiagnosticCoveredSwitchDefault
// ReSharper disable CppClangTidyClangDiagnosticImplicitFallthrough

#include "zydis/Zycore/LibC.h"
#include "zydis/Zydis/Encoder.h"
#include "zydis/Zydis/Utils.h"
#include "zydis/Zydis/Internal/EncoderData.h"
#include "zydis/Zydis/Internal/SharedData.h"

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constants                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_OPSIZE_MAP_BYTEOP                 1
#define ZYDIS_OPSIZE_MAP_DEFAULT64              4
#define ZYDIS_OPSIZE_MAP_FORCE64                5
#define ZYDIS_ADSIZE_MAP_IGNORED                1
#define ZYDIS_LEGACY_SEGMENTS                   (ZYDIS_ATTRIB_HAS_SEGMENT_CS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_SS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_DS | \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT_ES)
#define ZYDIS_ENCODABLE_PREFIXES_NO_SEGMENTS    (ZYDIS_ENCODABLE_PREFIXES ^ \
                                                 ZYDIS_ATTRIB_HAS_SEGMENT)

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Internal enums and types                                                                       */
/* ============================================================================================== */

/**
 * Usage of `REX.W` prefix makes it impossible to use some byte-sized registers. Values of this
 * enum are used to track and facilitate enforcement of these restrictions.
 */
typedef enum ZydisEncoderRexType_
{
    ZYDIS_REX_TYPE_UNKNOWN,
    ZYDIS_REX_TYPE_REQUIRED,
    ZYDIS_REX_TYPE_FORBIDDEN,

    /**
     * Maximum value of this enum.
     */
    ZYDIS_REX_TYPE_MAX_VALUE = ZYDIS_REX_TYPE_FORBIDDEN,
    /**
     * The minimum number of bits required to represent all values of this enum.
     */
    ZYDIS_REX_TYPE_REQUIRED_BITS = ZYAN_BITS_TO_REPRESENT(ZYDIS_REX_TYPE_MAX_VALUE)
} ZydisEncoderRexType;

/**
 * Primary structure used during instruction matching phase. Once filled it contains information
 * about matched instruction definition and some values deduced from encoder request. It gets
 * converted to `ZydisEncoderInstruction` during instruction building phase.
 */
typedef struct ZydisEncoderInstructionMatch_
{
    /**
     * A pointer to the `ZydisEncoderRequest` instance.
     */
    const ZydisEncoderRequest *request;
    /**
     * A pointer to the `ZydisEncodableInstruction` instance.
     */
    const ZydisEncodableInstruction *definition;
    /**
     * A pointer to the `ZydisInstructionDefinition` instance.
     */
    const ZydisInstructionDefinition *base_definition;
    /**
     * A pointer to the `ZydisOperandDefinition` array.
     */
    const ZydisOperandDefinition *operands;
    /**
     * Encodable attributes for this instruction.
     */
    ZydisInstructionAttributes attributes;
    /**
     * Effective operand size attribute.
     */
    ZyanU8 eosz;
    /**
     * Effective address size attribute.
     */
    ZyanU8 easz;
    /**
     * Effective displacement size.
     */
    ZyanU8 disp_size;
    /**
     * Effective immediate size.
     */
    ZyanU8 imm_size;
    /**
     * Exponent of compressed displacement scale factor (2^cd8_scale)
     */
    ZyanU8 cd8_scale;
    /**
     * `REX` prefix constraints.
     */
    ZydisEncoderRexType rex_type;
    /**
     * True for special cases where operand size attribute must be lower than 64 bits.
     */
    ZyanBool eosz64_forbidden;
    /**
     * True when instruction definition has relative operand (used for branching instructions).
     */
    ZyanBool has_rel_operand;
} ZydisEncoderInstructionMatch;

/**
 * Encapsulates information about writable buffer.
 */
typedef struct ZydisEncoderBuffer_
{
    /**
     * A pointer to actual data buffer.
     */
    ZyanU8 *buffer;
    /**
     * Size of this buffer.
     */
    ZyanUSize size;
    /**
     * Current write offset.
     */
    ZyanUSize offset;
} ZydisEncoderBuffer;

/**
 * Low-level instruction representation. Once filled this structure contains all information
 * required for final instruction emission phase.
 */
typedef struct ZydisEncoderInstruction_
{
    /**
     * Encodable attributes for this instruction.
     */
    ZydisInstructionAttributes attributes;
    /**
     * The instruction encoding.
     */
    ZydisInstructionEncoding encoding;
    /**
     * The opcode map.
     */
    ZydisOpcodeMap opcode_map;
    /**
     * The opcode.
     */
    ZyanU8 opcode;
    /**
     * The `vvvv` field (`VEX`, `EVEX`, `MVEX`, `XOP`).
     */
    ZyanU8 vvvv;
    /**
     * The `sss` field (`MVEX`).
     */
    ZyanU8 sss;
    /**
     * The mask register ID.
     */
    ZyanU8 mask;
    /**
     * The vector length.
     */
    ZyanU8 vector_length;
    /**
     * The `mod` component of Mod/RM byte.
     */
    ZyanU8 mod;
    /**
     * The `reg` component of Mod/RM byte.
     */
    ZyanU8 reg;
    /**
     * The `rm` component of Mod/RM byte.
     */
    ZyanU8 rm;
    /**
     * The scale component of SIB byte.
     */
    ZyanU8 scale;
    /**
     * The index component of SIB byte.
     */
    ZyanU8 index;
    /**
     * The base component of SIB byte.
     */
    ZyanU8 base;
    /**
     * The `REX.W` bit.
     */
    ZyanBool rex_w;
    /**
     * True if using zeroing mask (`EVEX`).
     */
    ZyanBool zeroing;
    /**
     * True if using eviction hint (`MVEX`).
     */
    ZyanBool eviction_hint;
    /**
     * Size of displacement value.
     */
    ZyanU8 disp_size;
    /**
     * Size of immediate value.
     */
    ZyanU8 imm_size;
    /**
     * The displacement value.
     */
    ZyanU64 disp;
    /**
     * The immediate value.
     */
    ZyanU64 imm;
} ZydisEncoderInstruction;

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/**
 * Converts `ZydisInstructionEncoding` to `ZydisEncodableEncoding`.
 *
 * @param   encoding `ZydisInstructionEncoding` value to convert.
 *
 * @return  Equivalent `ZydisEncodableEncoding` value.
 */
static ZydisEncodableEncoding ZydisGetEncodableEncoding(ZydisInstructionEncoding encoding)
{
    static const ZydisEncodableEncoding encoding_lookup[6] =
    {
        ZYDIS_ENCODABLE_ENCODING_LEGACY,
        ZYDIS_ENCODABLE_ENCODING_3DNOW,
        ZYDIS_ENCODABLE_ENCODING_XOP,
        ZYDIS_ENCODABLE_ENCODING_VEX,
        ZYDIS_ENCODABLE_ENCODING_EVEX,
        ZYDIS_ENCODABLE_ENCODING_MVEX,
    };
    ZYAN_ASSERT((ZyanUSize)encoding <= ZYDIS_INSTRUCTION_ENCODING_MAX_VALUE);
    return encoding_lookup[encoding];
}

/**
 * Converts `ZydisMachineMode` to default stack width value expressed in bits.
 *
 * @param   machine_mode `ZydisMachineMode` value to convert.
 *
 * @return  Stack width for requested machine mode.
 */
static ZyanU8 ZydisGetMachineModeWidth(ZydisMachineMode machine_mode)
{
    switch (machine_mode)
    {
    case ZYDIS_MACHINE_MODE_REAL_16:
    case ZYDIS_MACHINE_MODE_LEGACY_16:
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
        return 16;
    case ZYDIS_MACHINE_MODE_LEGACY_32:
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
        return 32;
    case ZYDIS_MACHINE_MODE_LONG_64:
        return 64;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Converts `ZydisAddressSizeHint` to address size expressed in bits.
 *
 * @param   hint Address size hint.
 *
 * @return  Address size in bits.
 */
static ZyanU8 ZydisGetAszFromHint(ZydisAddressSizeHint hint)
{
    ZYAN_ASSERT((ZyanUSize)hint <= ZYDIS_ADDRESS_SIZE_HINT_MAX_VALUE);
    static const ZyanU8 lookup[ZYDIS_ADDRESS_SIZE_HINT_MAX_VALUE + 1] = { 0, 16, 32, 64 };
    return lookup[hint];
}

/**
 * Converts `ZydisOperandSizeHint` to operand size expressed in bits.
 *
 * @param   hint Operand size hint.
 *
 * @return  Operand size in bits.
 */
static ZyanU8 ZydisGetOszFromHint(ZydisOperandSizeHint hint)
{
    ZYAN_ASSERT((ZyanUSize)hint <= ZYDIS_OPERAND_SIZE_HINT_MAX_VALUE);
    static const ZyanU8 lookup[ZYDIS_OPERAND_SIZE_HINT_MAX_VALUE + 1] = { 0, 8, 16, 32, 64 };
    return lookup[hint];
}

/**
 * Calculates effective operand size.
 *
 * @param   match            A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   size_table       Array of possible size values for different operand sizes.
 * @param   desired_size     Operand size requested by caller.
 * @param   exact_match_mode True if desired_size must be matched exactly, false when
 *                           "not lower than" matching is desired.
 *
 * @return  Effective operand size in bits.
 */
static ZyanU8 ZydisGetOperandSizeFromElementSize(ZydisEncoderInstructionMatch *match,
    const ZyanU16 *size_table, ZyanU16 desired_size, ZyanBool exact_match_mode)
{
    if ((match->base_definition->operand_size_map == ZYDIS_OPSIZE_MAP_DEFAULT64) &&
        (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64))
    {
        if ((exact_match_mode && (size_table[2] == desired_size)) ||
            (!exact_match_mode && (size_table[2] >= desired_size)))
        {
            return 64;
        }
        else if (size_table[0] == desired_size)
        {
            return 16;
        }
    }
    else if ((match->base_definition->operand_size_map == ZYDIS_OPSIZE_MAP_FORCE64) &&
             (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64))
    {
        if (size_table[2] == desired_size)
        {
            return 64;
        }
    }
    else
    {
        static const ZyanI8 eosz_priority_lookup[4][3] =
        {
            {  0,  1, -1 },
            {  1,  0, -1 },
            {  1,  2,  0 },
        };
        const ZyanU8 eosz_index = ZydisGetMachineModeWidth(match->request->machine_mode) >> 5;
        for (int i = 0; i < 3; ++i)
        {
            const ZyanI8 eosz_candidate = eosz_priority_lookup[eosz_index][i];
            if ((eosz_candidate == -1) ||
                !(match->definition->operand_sizes & (1 << eosz_candidate)))
            {
                continue;
            }
            if ((exact_match_mode && (size_table[eosz_candidate] == desired_size)) ||
                (!exact_match_mode && (size_table[eosz_candidate] >= desired_size)))
            {
                return 16 << eosz_candidate;
            }
        }
    }

    return 0;
}

/**
 * Calculates effective immediate size.
 *
 * @param   match        A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   size_table   Array of possible size values for different operand sizes.
 * @param   min_imm_size Minimum immediate size.
 *
 * @return  Effective operand size in bits.
 */
static ZyanU8 ZydisGetScaledImmSize(ZydisEncoderInstructionMatch *match, const ZyanU16 *size_table,
    ZyanU8 min_imm_size)
{
    if (match->eosz == 0)
    {
        match->eosz = ZydisGetOperandSizeFromElementSize(match, size_table, min_imm_size,
            ZYAN_FALSE);
        return match->eosz != 0 ? (ZyanU8)size_table[match->eosz >> 5] : 0;
    }

    const ZyanU8 index = match->eosz >> 5;
    return size_table[index] >= min_imm_size ? (ZyanU8)size_table[index] : 0;
}

/**
 * Calculates size of smallest integral type able to represent provided signed value.
 *
 * @param   imm Immediate to be represented.
 *
 * @return  Size of smallest integral type able to represent provided signed value.
 */
static ZyanU8 ZydisGetSignedImmSize(ZyanI64 imm)
{
    if (imm >= ZYAN_INT8_MIN && imm <= ZYAN_INT8_MAX)
    {
        return 8;
    }
    if (imm >= ZYAN_INT16_MIN && imm <= ZYAN_INT16_MAX)
    {
        return 16;
    }
    if (imm >= ZYAN_INT32_MIN && imm <= ZYAN_INT32_MAX)
    {
        return 32;
    }

    return 64;
}

/**
 * Calculates size of smallest integral type able to represent provided unsigned value.
 *
 * @param   imm Immediate to be represented.
 *
 * @return  Size of smallest integral type able to represent provided unsigned value.
 */
static ZyanU8 ZydisGetUnsignedImmSize(ZyanU64 imm)
{
    if (imm <= ZYAN_UINT8_MAX)
    {
        return 8;
    }
    if (imm <= ZYAN_UINT16_MAX)
    {
        return 16;
    }
    if (imm <= ZYAN_UINT32_MAX)
    {
        return 32;
    }

    return 64;
}

/**
 * Checks if operand encoding encodes a signed immediate value.
 *
 * @param   encoding Operand encoding for immediate value.
 *
 * @return  True for encodings that represent signed values, false otherwise.
 */
static ZyanBool ZydisIsImmSigned(ZydisOperandEncoding encoding)
{
    switch (encoding)
    {
    case ZYDIS_OPERAND_ENCODING_SIMM8:
    case ZYDIS_OPERAND_ENCODING_SIMM16:
    case ZYDIS_OPERAND_ENCODING_SIMM32:
    case ZYDIS_OPERAND_ENCODING_SIMM64:
    case ZYDIS_OPERAND_ENCODING_SIMM16_32_64:
    case ZYDIS_OPERAND_ENCODING_SIMM32_32_64:
    case ZYDIS_OPERAND_ENCODING_SIMM16_32_32:
    case ZYDIS_OPERAND_ENCODING_JIMM8:
    case ZYDIS_OPERAND_ENCODING_JIMM16:
    case ZYDIS_OPERAND_ENCODING_JIMM32:
    case ZYDIS_OPERAND_ENCODING_JIMM64:
    case ZYDIS_OPERAND_ENCODING_JIMM16_32_64:
    case ZYDIS_OPERAND_ENCODING_JIMM32_32_64:
    case ZYDIS_OPERAND_ENCODING_JIMM16_32_32:
        return ZYAN_TRUE;
    case ZYDIS_OPERAND_ENCODING_DISP8:
    case ZYDIS_OPERAND_ENCODING_DISP16:
    case ZYDIS_OPERAND_ENCODING_DISP32:
    case ZYDIS_OPERAND_ENCODING_DISP64:
    case ZYDIS_OPERAND_ENCODING_DISP16_32_64:
    case ZYDIS_OPERAND_ENCODING_DISP32_32_64:
    case ZYDIS_OPERAND_ENCODING_DISP16_32_32:
    case ZYDIS_OPERAND_ENCODING_UIMM8:
    case ZYDIS_OPERAND_ENCODING_UIMM16:
    case ZYDIS_OPERAND_ENCODING_UIMM32:
    case ZYDIS_OPERAND_ENCODING_UIMM64:
    case ZYDIS_OPERAND_ENCODING_UIMM16_32_64:
    case ZYDIS_OPERAND_ENCODING_UIMM32_32_64:
    case ZYDIS_OPERAND_ENCODING_UIMM16_32_32:
    case ZYDIS_OPERAND_ENCODING_IS4:
        return ZYAN_FALSE;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Calculates effective immediate size.
 *
 * @param   match   A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   imm     Immediate value to encode.
 * @param   def_op  Operand definition for immediate operand.
 *
 * @return  Effective operand size in bits (0 if function failed).
 */
static ZyanU8 ZydisGetEffectiveImmSize(ZydisEncoderInstructionMatch *match, ZyanI64 imm,
    const ZydisOperandDefinition *def_op)
{
    ZyanU8 eisz = 0;
    ZyanU8 min_size = ZydisIsImmSigned((ZydisOperandEncoding)def_op->op.encoding)
        ? ZydisGetSignedImmSize(imm)
        : ZydisGetUnsignedImmSize((ZyanU64)imm);

    switch (def_op->op.encoding)
    {
    case ZYDIS_OPERAND_ENCODING_UIMM8:
    case ZYDIS_OPERAND_ENCODING_SIMM8:
        eisz = 8;
        break;
    case ZYDIS_OPERAND_ENCODING_IS4:
        ZYAN_ASSERT(def_op->element_type == ZYDIS_IELEMENT_TYPE_UINT8);
        eisz = ((ZyanU64)imm <= 15) ? 8 : 0;
        break;
    case ZYDIS_OPERAND_ENCODING_UIMM16:
    case ZYDIS_OPERAND_ENCODING_SIMM16:
        eisz = 16;
        break;
    case ZYDIS_OPERAND_ENCODING_UIMM32:
    case ZYDIS_OPERAND_ENCODING_SIMM32:
        eisz = 32;
        break;
    case ZYDIS_OPERAND_ENCODING_UIMM64:
    case ZYDIS_OPERAND_ENCODING_SIMM64:
        eisz = 64;
        break;
    case ZYDIS_OPERAND_ENCODING_UIMM16_32_64:
    case ZYDIS_OPERAND_ENCODING_SIMM16_32_64:
    {
        static const ZyanU16 simm16_32_64_sizes[3] = { 16, 32, 64 };
        return ZydisGetScaledImmSize(match, simm16_32_64_sizes, min_size);
    }
    case ZYDIS_OPERAND_ENCODING_UIMM32_32_64:
    case ZYDIS_OPERAND_ENCODING_SIMM32_32_64:
    {
        static const ZyanU16 simm32_32_64_sizes[3] = { 32, 32, 64 };
        return ZydisGetScaledImmSize(match, simm32_32_64_sizes, min_size);
    }
    case ZYDIS_OPERAND_ENCODING_UIMM16_32_32:
    case ZYDIS_OPERAND_ENCODING_SIMM16_32_32:
    {
        static const ZyanU16 simm16_32_32_sizes[3] = { 16, 32, 32 };
        return ZydisGetScaledImmSize(match, simm16_32_32_sizes, min_size);
    }
    case ZYDIS_OPERAND_ENCODING_DISP16_32_64:
        ZYAN_ASSERT(match->easz == 0);
        if (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
        {
            if (min_size < 32)
            {
                min_size = 32;
            }
            if (min_size == 32 || min_size == 64)
            {
                match->easz = eisz = min_size;
            }
        }
        else
        {
            if (min_size < 16)
            {
                min_size = 16;
            }
            if (min_size == 16 || min_size == 32)
            {
                match->easz = eisz = min_size;
            }
        }
        break;
    case ZYDIS_OPERAND_ENCODING_JIMM8:
    case ZYDIS_OPERAND_ENCODING_JIMM16:
    case ZYDIS_OPERAND_ENCODING_JIMM32:
    case ZYDIS_OPERAND_ENCODING_JIMM64:
    {
        ZyanU8 jimm_index = def_op->op.encoding - ZYDIS_OPERAND_ENCODING_JIMM8;
        if ((match->request->branch_width != ZYDIS_BRANCH_WIDTH_NONE) &&
            (match->request->branch_width != (ZydisBranchWidth)(ZYDIS_BRANCH_WIDTH_8 + jimm_index)))
        {
            return 0;
        }
        eisz = 8 << jimm_index;
        break;
    }
    case ZYDIS_OPERAND_ENCODING_JIMM16_32_32:
        switch (match->request->branch_width)
        {
        case ZYDIS_BRANCH_WIDTH_NONE:
        {
            static const ZyanU16 jimm16_32_32_sizes[3] = { 16, 32, 32 };
            return ZydisGetScaledImmSize(match, jimm16_32_32_sizes, min_size);
        }
        case ZYDIS_BRANCH_WIDTH_16:
            eisz = 16;
            break;
        case ZYDIS_BRANCH_WIDTH_32:
            eisz = 32;
            break;
        case ZYDIS_BRANCH_WIDTH_8:
        case ZYDIS_BRANCH_WIDTH_64:
            return 0;
        default:
            ZYAN_UNREACHABLE;
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    return eisz >= min_size ? eisz : 0;
}

/**
 * Checks if register width is compatible with effective operand size.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   reg_width   Register width in bits.
 *
 * @return  True if width is compatible, false otherwise.
 */
static ZyanBool ZydisCheckOsz(ZydisEncoderInstructionMatch *match, ZydisRegisterWidth reg_width)
{
    ZYAN_ASSERT(reg_width <= ZYAN_UINT8_MAX);
    if (match->eosz == 0)
    {
        if (reg_width == 8)
        {
            return ZYAN_FALSE;
        }
        match->eosz = (ZyanU8)reg_width;
        return ZYAN_TRUE;
    }

    return match->eosz == (ZyanU8)reg_width ? ZYAN_TRUE : ZYAN_FALSE;
}

/**
 * Checks if register width is compatible with effective address size.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   reg_width   Register width in bits.
 *
 * @return  True if width is compatible, false otherwise.
 */
static ZyanBool ZydisCheckAsz(ZydisEncoderInstructionMatch *match, ZydisRegisterWidth reg_width)
{
    ZYAN_ASSERT(reg_width <= ZYAN_UINT8_MAX);
    if (match->easz == 0)
    {
        if ((match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
            (reg_width == 16))
        {
            return ZYAN_FALSE;
        }
        match->easz = (ZyanU8)reg_width;
        return ZYAN_TRUE;
    }

    return match->easz == (ZyanU8)reg_width ? ZYAN_TRUE : ZYAN_FALSE;
}

/**
 * Checks if specified register is valid for provided register class, encoding and machine mode.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   reg         `ZydisRegister` value.
 * @param   reg_class   Register class.
 *
 * @return  True if register value is allowed, false otherwise.
 */
static ZyanBool ZydisIsRegisterAllowed(ZydisEncoderInstructionMatch *match, ZydisRegister reg,
    ZydisRegisterClass reg_class)
{
    const ZyanI8 reg_id = ZydisRegisterGetId(reg);
    ZYAN_ASSERT(reg_id >= 0 && reg_id <= 31);
    if (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
    {
        if ((match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_EVEX) &&
            (match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_MVEX) &&
            (reg_class != ZYDIS_REGCLASS_GPR8) &&
            (reg_id >= 16))
        {
            return ZYAN_FALSE;
        }
    }
    else
    {
        if (reg_class == ZYDIS_REGCLASS_GPR64)
        {
            return ZYAN_FALSE;
        }
        if (reg_id >= 8)
        {
            return ZYAN_FALSE;
        }
    }

    return ZYAN_TRUE;
}

/**
 * Checks if specified scale value is valid for use with SIB addressing.
 *
 * @param   scale Scale value.
 *
 * @return  True if value is valid, false otherwise.
 */
static ZyanBool ZydisIsScaleValid(ZyanU8 scale)
{
    switch (scale)
    {
    case 0:
    case 1:
    case 2:
    case 4:
    case 8:
        return ZYAN_TRUE;
    default:
        return ZYAN_FALSE;
    }
}

/**
 * Enforces register usage constraints associated with usage of `REX` prefix.
 *
 * @param   match               A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   reg                 `ZydisRegister` value.
 * @param   addressing_mode     True if checked address is used for address calculations. This
 *                              implies more permissive checks.
 *
 * @return  True if register usage is allowed, false otherwise.
 */
static ZyanBool ZydisValidateRexType(ZydisEncoderInstructionMatch *match, ZydisRegister reg,
    ZyanBool addressing_mode)
{
    switch (reg)
    {
    case ZYDIS_REGISTER_AL:
    case ZYDIS_REGISTER_CL:
    case ZYDIS_REGISTER_DL:
    case ZYDIS_REGISTER_BL:
        return ZYAN_TRUE;
    case ZYDIS_REGISTER_AH:
    case ZYDIS_REGISTER_CH:
    case ZYDIS_REGISTER_DH:
    case ZYDIS_REGISTER_BH:
        if (match->rex_type == ZYDIS_REX_TYPE_UNKNOWN)
        {
            match->rex_type = ZYDIS_REX_TYPE_FORBIDDEN;
        }
        else if (match->rex_type == ZYDIS_REX_TYPE_REQUIRED)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_REGISTER_SPL:
    case ZYDIS_REGISTER_BPL:
    case ZYDIS_REGISTER_SIL:
    case ZYDIS_REGISTER_DIL:
    case ZYDIS_REGISTER_R8B:
    case ZYDIS_REGISTER_R9B:
    case ZYDIS_REGISTER_R10B:
    case ZYDIS_REGISTER_R11B:
    case ZYDIS_REGISTER_R12B:
    case ZYDIS_REGISTER_R13B:
    case ZYDIS_REGISTER_R14B:
    case ZYDIS_REGISTER_R15B:
        if (match->rex_type == ZYDIS_REX_TYPE_UNKNOWN)
        {
            match->rex_type = ZYDIS_REX_TYPE_REQUIRED;
        }
        else if (match->rex_type == ZYDIS_REX_TYPE_FORBIDDEN)
        {
            return ZYAN_FALSE;
        }
        break;
    default:
        if ((ZydisRegisterGetId(reg) > 7) ||
            (!addressing_mode && (ZydisRegisterGetClass(reg) == ZYDIS_REGCLASS_GPR64)))
        {
            if (match->rex_type == ZYDIS_REX_TYPE_UNKNOWN)
            {
                match->rex_type = ZYDIS_REX_TYPE_REQUIRED;
            }
            else if (match->rex_type == ZYDIS_REX_TYPE_FORBIDDEN)
            {
                return ZYAN_FALSE;
            }
        }
        break;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if specified register is valid for use with SIB addressing.
 *
 * @param   match          A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   reg_class      Register class.
 * @param   reg            `ZydisRegister` value.
 *
 * @return  True if register value is allowed, false otherwise.
 */
static ZyanBool ZydisIsValidAddressingClass(ZydisEncoderInstructionMatch *match,
    ZydisRegisterClass reg_class, ZydisRegister reg)
{
    ZyanBool result;
    const ZyanBool is_64 = (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64);
    switch (reg_class)
    {
    case ZYDIS_REGCLASS_INVALID:
        return ZYAN_TRUE;
    case ZYDIS_REGCLASS_GPR16:
        result = !is_64;
        break;
    case ZYDIS_REGCLASS_GPR32:
        result = is_64 || ZydisRegisterGetId(reg) < 8;
        break;
    case ZYDIS_REGCLASS_GPR64:
        result = is_64;
        break;
    default:
        return ZYAN_FALSE;
    }

    return result && ZydisValidateRexType(match, reg, ZYAN_TRUE);
}

/**
 * Helper function that determines correct `ModR/M.RM` value for 16-bit addressing mode.
 *
 * @param   base   `ZydisRegister` used as `SIB.base`.
 * @param   index  `ZydisRegister` used as `SIB.index`.
 *
 * @return  `ModR/M.RM` value (-1 if function failed).
 */
static ZyanI8 ZydisGetRm16(ZydisRegister base, ZydisRegister index)
{
    static const ZydisRegister modrm16_lookup[8][2] =
    {
        { ZYDIS_REGISTER_BX, ZYDIS_REGISTER_SI },
        { ZYDIS_REGISTER_BX, ZYDIS_REGISTER_DI },
        { ZYDIS_REGISTER_BP, ZYDIS_REGISTER_SI },
        { ZYDIS_REGISTER_BP, ZYDIS_REGISTER_DI },
        { ZYDIS_REGISTER_SI, ZYDIS_REGISTER_NONE },
        { ZYDIS_REGISTER_DI, ZYDIS_REGISTER_NONE },
        { ZYDIS_REGISTER_BP, ZYDIS_REGISTER_NONE },
        { ZYDIS_REGISTER_BX, ZYDIS_REGISTER_NONE },
    };
    for (ZyanI8 i = 0; i < (ZyanI8)ZYAN_ARRAY_LENGTH(modrm16_lookup); ++i)
    {
        if ((modrm16_lookup[i][0] == base) &&
            (modrm16_lookup[i][1] == index))
        {
            return i;
        }
    }

    return -1;
}

/**
 * Encodes `MVEX.sss` field for specified broadcast mode.
 *
 * @param   broadcast Broadcast mode.
 *
 * @return  Corresponding `MVEX.sss` value.
 */
static ZyanU8 ZydisEncodeMvexBroadcastMode(ZydisBroadcastMode broadcast)
{
    switch (broadcast)
    {
    case ZYDIS_BROADCAST_MODE_INVALID:
        return 0;
    case ZYDIS_BROADCAST_MODE_1_TO_16:
    case ZYDIS_BROADCAST_MODE_1_TO_8:
        return 1;
    case ZYDIS_BROADCAST_MODE_4_TO_16:
    case ZYDIS_BROADCAST_MODE_4_TO_8:
        return 2;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Encodes `MVEX.sss` field for specified conversion mode.
 *
 * @param   conversion Conversion mode.
 *
 * @return  Corresponding `MVEX.sss` value.
 */
static ZyanU8 ZydisEncodeMvexConversionMode(ZydisConversionMode conversion)
{
    switch (conversion)
    {
    case ZYDIS_CONVERSION_MODE_INVALID:
        return 0;
    case ZYDIS_CONVERSION_MODE_FLOAT16:
        return 3;
    case ZYDIS_CONVERSION_MODE_UINT8:
        return 4;
    case ZYDIS_CONVERSION_MODE_SINT8:
        return 5;
    case ZYDIS_CONVERSION_MODE_UINT16:
        return 6;
    case ZYDIS_CONVERSION_MODE_SINT16:
        return 7;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Determines scale factor for compressed 8-bit displacement (`EVEX` instructions only).
 *
 * @param   match   A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  log2(scale factor)
 */
static ZyanU8 ZydisGetCompDispScaleEvex(const ZydisEncoderInstructionMatch *match)
{
    const ZydisInstructionDefinitionEVEX *evex_def =
        (const ZydisInstructionDefinitionEVEX *)match->base_definition;

    ZYAN_ASSERT(match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX);
    ZYAN_ASSERT(evex_def->tuple_type);
    ZYAN_ASSERT(evex_def->element_size);
    const ZyanU8 vector_length = match->definition->vector_length - ZYDIS_VECTOR_LENGTH_128;
    static const ZyanU8 size_indexes[ZYDIS_IELEMENT_SIZE_MAX_VALUE + 1] =
    {
        0, 0, 0, 1, 2, 4
    };
    ZYAN_ASSERT(evex_def->element_size < ZYAN_ARRAY_LENGTH(size_indexes));
    const ZyanU8 size_index = size_indexes[evex_def->element_size];
    switch (evex_def->tuple_type)
    {
    case ZYDIS_TUPLETYPE_FV:
    {
        static const ZyanU8 scales[2][3][3] =
        {
            /*B0*/ { /*16*/ { 4, 5, 6 }, /*32*/ { 4, 5, 6 }, /*64*/ { 4, 5, 6 } },
            /*B1*/ { /*16*/ { 1, 1, 1 }, /*32*/ { 2, 2, 2 }, /*64*/ { 3, 3, 3 } }
        };
        const ZyanU8 broadcast = match->request->evex.broadcast ? 1 : 0;
        ZYAN_ASSERT(size_index < 3);
        return scales[broadcast][size_index][vector_length];
    }
    case ZYDIS_TUPLETYPE_HV:
    {
        static const ZyanU8 scales[2][2][3] =
        {
            /*B0*/ { /*16*/ {  3, 4, 5 }, /*32*/ {  3, 4, 5 } },
            /*B1*/ { /*16*/ {  1, 1, 1 }, /*32*/ {  2, 2, 2 } }
        };
        const ZyanU8 broadcast = match->request->evex.broadcast ? 1 : 0;
        ZYAN_ASSERT(size_index < 3);
        return scales[broadcast][size_index][vector_length];
    }
    case ZYDIS_TUPLETYPE_FVM:
    {
        static const ZyanU8 scales[3] =
        {
            4, 5, 6
        };
        return scales[vector_length];
    }
    case ZYDIS_TUPLETYPE_GSCAT:
    case ZYDIS_TUPLETYPE_T1S:
    {
        static const ZyanU8 scales[6] =
        {
            /*   */ 0,
            /*  8*/ 0,
            /* 16*/ 1,
            /* 32*/ 2,
            /* 64*/ 3,
            /*128*/ 4
        };
        ZYAN_ASSERT(evex_def->element_size < ZYAN_ARRAY_LENGTH(scales));
        return scales[evex_def->element_size];
    }
    case ZYDIS_TUPLETYPE_T1F:
    {
        static const ZyanU8 scales[3] =
        {
            /* 16*/ 1,
            /* 32*/ 2,
            /* 64*/ 3
        };
        ZYAN_ASSERT(size_index < 3);
        return scales[size_index];
    }
    case ZYDIS_TUPLETYPE_T1_4X:
        return 4;
    case ZYDIS_TUPLETYPE_T2:
        return match->definition->rex_w ? 4 : 3;
    case ZYDIS_TUPLETYPE_T4:
        return match->definition->rex_w ? 5 : 4;
    case ZYDIS_TUPLETYPE_T8:
        return 5;
    case ZYDIS_TUPLETYPE_HVM:
    {
        static const ZyanU8 scales[3] =
        {
            3, 4, 5
        };
        return scales[vector_length];
    }
    case ZYDIS_TUPLETYPE_QVM:
    {
        static const ZyanU8 scales[3] =
        {
            2, 3, 4
        };
        return scales[vector_length];
    }
    case ZYDIS_TUPLETYPE_OVM:
    {
        static const ZyanU8 scales[3] =
        {
            1, 2, 3
        };
        return scales[vector_length];
    }
    case ZYDIS_TUPLETYPE_M128:
        return 4;
    case ZYDIS_TUPLETYPE_DUP:
    {
        static const ZyanU8 scales[3] =
        {
            3, 5, 6
        };
        return scales[vector_length];
    }
    case ZYDIS_TUPLETYPE_QUARTER:
    {
        static const ZyanU8 scales[2][3] =
        {
            /*B0*/ { 2, 3, 4 },
            /*B1*/ { 1, 1, 1 }
        };
        const ZyanU8 broadcast = match->request->evex.broadcast ? 1 : 0;
        return scales[broadcast][vector_length];
    }
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Determines scale factor for compressed 8-bit displacement (`MVEX` instructions only).
 *
 * @param   match   A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  log2(scale factor)
 */
static ZyanU8 ZydisGetCompDispScaleMvex(const ZydisEncoderInstructionMatch *match)
{
    const ZydisInstructionDefinitionMVEX *mvex_def =
        (const ZydisInstructionDefinitionMVEX *)match->base_definition;

    ZyanU8 index = mvex_def->has_element_granularity;
    ZYAN_ASSERT(!index || !mvex_def->broadcast);
    if (!index && mvex_def->broadcast)
    {
        switch (mvex_def->broadcast)
        {
        case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_8:
        case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_16:
            index = 1;
            break;
        case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_8:
        case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_16:
            index = 2;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
    }

    const ZyanU8 sss = ZydisEncodeMvexBroadcastMode(match->request->mvex.broadcast) |
                       ZydisEncodeMvexConversionMode(match->request->mvex.conversion);
    switch (mvex_def->functionality)
    {
    case ZYDIS_MVEX_FUNC_IGNORED:
    case ZYDIS_MVEX_FUNC_INVALID:
    case ZYDIS_MVEX_FUNC_RC:
    case ZYDIS_MVEX_FUNC_SAE:
    case ZYDIS_MVEX_FUNC_SWIZZLE_32:
    case ZYDIS_MVEX_FUNC_SWIZZLE_64:
        return 0;
    case ZYDIS_MVEX_FUNC_F_32:
    case ZYDIS_MVEX_FUNC_I_32:
    case ZYDIS_MVEX_FUNC_F_64:
    case ZYDIS_MVEX_FUNC_I_64:
        return 6;
    case ZYDIS_MVEX_FUNC_SF_32:
    case ZYDIS_MVEX_FUNC_SF_32_BCST:
    case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
    case ZYDIS_MVEX_FUNC_UF_32:
    {
        static const ZyanU8 lookup[3][8] =
        {
            { 6, 2, 4, 5, 4, 4, 5, 5 },
            { 2, 0, 0, 1, 0, 0, 1, 1 },
            { 4, 0, 0, 3, 2, 2, 3, 3 }
        };
        ZYAN_ASSERT(sss < ZYAN_ARRAY_LENGTH(lookup[index]));
        return lookup[index][sss];
    }
    case ZYDIS_MVEX_FUNC_SI_32:
    case ZYDIS_MVEX_FUNC_UI_32:
    case ZYDIS_MVEX_FUNC_SI_32_BCST:
    case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
    {
        static const ZyanU8 lookup[3][8] =
        {
            { 6, 2, 4, 0, 4, 4, 5, 5 },
            { 2, 0, 0, 0, 0, 0, 1, 1 },
            { 4, 0, 0, 0, 2, 2, 3, 3 }
        };
        ZYAN_ASSERT(sss < ZYAN_ARRAY_LENGTH(lookup[index]));
        return lookup[index][sss];
    }
    case ZYDIS_MVEX_FUNC_SF_64:
    case ZYDIS_MVEX_FUNC_UF_64:
    case ZYDIS_MVEX_FUNC_SI_64:
    case ZYDIS_MVEX_FUNC_UI_64:
    {
        static const ZyanU8 lookup[3][3] =
        {
            { 6, 3, 5 },
            { 3, 0, 0 },
            { 5, 0, 0 }
        };
        ZYAN_ASSERT(sss < ZYAN_ARRAY_LENGTH(lookup[index]));
        return lookup[index][sss];
    }
    case ZYDIS_MVEX_FUNC_DF_32:
    case ZYDIS_MVEX_FUNC_DI_32:
    {
        static const ZyanU8 lookup[2][8] =
        {
            { 6, 0, 0, 5, 4, 4, 5, 5 },
            { 2, 0, 0, 1, 0, 0, 1, 1 }
        };
        ZYAN_ASSERT(index < 2);
        ZYAN_ASSERT(sss < ZYAN_ARRAY_LENGTH(lookup[index]));
        return lookup[index][sss];
    }
    case ZYDIS_MVEX_FUNC_DF_64:
    case ZYDIS_MVEX_FUNC_DI_64:
        ZYAN_ASSERT(index < 2);
        return index == 0 ? 6 : 3;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Determines scale factor for compressed 8-bit displacement.
 *
 * @param   match A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  log2(scale factor)
 */
static ZyanU8 ZydisGetCompDispScale(const ZydisEncoderInstructionMatch *match)
{
    switch (match->definition->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        return 0;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        return ZydisGetCompDispScaleEvex(match);
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        return ZydisGetCompDispScaleMvex(match);
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Checks if requested operand matches register operand from instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   user_op     Operand definition from `ZydisEncoderRequest` structure.
 * @param   def_op      Decoder's operand definition from current instruction definition.
 *
 * @return  True if operands match, false otherwise.
 */
static ZyanBool ZydisIsRegisterOperandCompatible(ZydisEncoderInstructionMatch *match,
    const ZydisEncoderOperand *user_op, const ZydisOperandDefinition *def_op)
{
    const ZydisRegisterClass reg_class = ZydisRegisterGetClass(user_op->reg.value);
    const ZydisRegisterWidth reg_width = ZydisRegisterClassGetWidth(match->request->machine_mode,
        reg_class);
    if (reg_width == 0)
    {
        return ZYAN_FALSE;
    }

    ZyanBool is4_expected_value = ZYAN_FALSE;
    switch (def_op->type)
    {
    case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_REG:
        switch (def_op->op.reg.type)
        {
        case ZYDIS_IMPLREG_TYPE_STATIC:
            if (def_op->op.reg.reg.reg != user_op->reg.value)
            {
                return ZYAN_FALSE;
            }
            break;
        case ZYDIS_IMPLREG_TYPE_GPR_OSZ:
            if ((reg_class != ZYDIS_REGCLASS_GPR8) &&
                (reg_class != ZYDIS_REGCLASS_GPR16) &&
                (reg_class != ZYDIS_REGCLASS_GPR32) &&
                (reg_class != ZYDIS_REGCLASS_GPR64))
            {
                return ZYAN_FALSE;
            }
            if (def_op->op.reg.reg.id != ZydisRegisterGetId(user_op->reg.value))
            {
                return ZYAN_FALSE;
            }
            if (!ZydisCheckOsz(match, reg_width))
            {
                return ZYAN_FALSE;
            }
            break;
        case ZYDIS_IMPLREG_TYPE_GPR_ASZ:
            if ((reg_class != ZYDIS_REGCLASS_GPR8) &&
                (reg_class != ZYDIS_REGCLASS_GPR16) &&
                (reg_class != ZYDIS_REGCLASS_GPR32) &&
                (reg_class != ZYDIS_REGCLASS_GPR64))
            {
                return ZYAN_FALSE;
            }
            if (def_op->op.reg.reg.id != ZydisRegisterGetId(user_op->reg.value))
            {
                return ZYAN_FALSE;
            }
            if (!ZydisCheckAsz(match, reg_width))
            {
                return ZYAN_FALSE;
            }
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR8:
        if (reg_class != ZYDIS_REGCLASS_GPR8)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisValidateRexType(match, user_op->reg.value, ZYAN_FALSE))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR16:
        if (reg_class != ZYDIS_REGCLASS_GPR16)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR32:
        if (reg_class != ZYDIS_REGCLASS_GPR32)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR64:
        if (reg_class != ZYDIS_REGCLASS_GPR64)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR16_32_64:
        if ((reg_class != ZYDIS_REGCLASS_GPR16) &&
            (reg_class != ZYDIS_REGCLASS_GPR32) &&
            (reg_class != ZYDIS_REGCLASS_GPR64))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisCheckOsz(match, reg_width))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisValidateRexType(match, user_op->reg.value, ZYAN_FALSE))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR32_32_64:
        if ((reg_class != ZYDIS_REGCLASS_GPR32) &&
            (reg_class != ZYDIS_REGCLASS_GPR64))
        {
            return ZYAN_FALSE;
        }
        if (match->eosz == 0)
        {
            if (reg_class == ZYDIS_REGCLASS_GPR64)
            {
                match->eosz = 64;
            }
            else
            {
                match->eosz64_forbidden = ZYAN_TRUE;
            }
        }
        else if (match->eosz != (ZyanU8)reg_width)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisValidateRexType(match, user_op->reg.value, ZYAN_FALSE))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR16_32_32:
        if ((reg_class != ZYDIS_REGCLASS_GPR16) &&
            (reg_class != ZYDIS_REGCLASS_GPR32))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisCheckOsz(match, reg_width))
        {
            if (match->eosz != 64 || reg_class != ZYDIS_REGCLASS_GPR32)
            {
                return ZYAN_FALSE;
            }
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_GPR_ASZ:
        if ((reg_class != ZYDIS_REGCLASS_GPR16) &&
            (reg_class != ZYDIS_REGCLASS_GPR32) &&
            (reg_class != ZYDIS_REGCLASS_GPR64))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisCheckAsz(match, reg_width))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_FPR:
        if (reg_class != ZYDIS_REGCLASS_X87)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_MMX:
        if (reg_class != ZYDIS_REGCLASS_MMX)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_XMM:
        if (reg_class != ZYDIS_REGCLASS_XMM)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        is4_expected_value = def_op->op.encoding == ZYDIS_OPERAND_ENCODING_IS4;
        break;
    case ZYDIS_SEMANTIC_OPTYPE_YMM:
        if (reg_class != ZYDIS_REGCLASS_YMM)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        is4_expected_value = def_op->op.encoding == ZYDIS_OPERAND_ENCODING_IS4;
        break;
    case ZYDIS_SEMANTIC_OPTYPE_ZMM:
        if (reg_class != ZYDIS_REGCLASS_ZMM)
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsRegisterAllowed(match, user_op->reg.value, reg_class))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_TMM:
        if (reg_class != ZYDIS_REGCLASS_TMM)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_BND:
        if (reg_class != ZYDIS_REGCLASS_BOUND)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_SREG:
        if (reg_class != ZYDIS_REGCLASS_SEGMENT)
        {
            return ZYAN_FALSE;
        }
        if ((def_op->actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) &&
            (user_op->reg.value == ZYDIS_REGISTER_CS))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_CR:
    {
        if (reg_class != ZYDIS_REGCLASS_CONTROL)
        {
            return ZYAN_FALSE;
        }
        static const ZyanU8 cr_lookup[16] =
        {
            1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0
        };
        const ZyanI8 reg_id = ZydisRegisterGetId(user_op->reg.value);
        if ((match->request->machine_mode != ZYDIS_MACHINE_MODE_LONG_64) &&
            (reg_id == 8))
        {
            return ZYAN_FALSE;
        }
        if (!cr_lookup[reg_id])
        {
            return ZYAN_FALSE;
        }
        break;
    }
    case ZYDIS_SEMANTIC_OPTYPE_DR:
        if (reg_class != ZYDIS_REGCLASS_DEBUG)
        {
            return ZYAN_FALSE;
        }
        if (user_op->reg.value >= ZYDIS_REGISTER_DR8)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_MASK:
        if (reg_class != ZYDIS_REGCLASS_MASK)
        {
            return ZYAN_FALSE;
        }

        // MVEX does not require similar policy check
        if ((match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) &&
            (def_op->op.encoding == ZYDIS_OPERAND_ENCODING_MASK))
        {
            const ZydisInstructionDefinitionEVEX *evex_def =
                (const ZydisInstructionDefinitionEVEX *)match->base_definition;
            ZYAN_ASSERT((evex_def->mask_policy != ZYDIS_MASK_POLICY_INVALID) &&
                        (evex_def->mask_policy != ZYDIS_MASK_POLICY_FORBIDDEN));
            if ((evex_def->mask_policy == ZYDIS_MASK_POLICY_REQUIRED) &&
                (user_op->reg.value == ZYDIS_REGISTER_K0))
            {
                return ZYAN_FALSE;
            }
            if ((evex_def->mask_policy == ZYDIS_MASK_POLICY_ALLOWED) &&
                (match->request->evex.zeroing_mask) &&
                (user_op->reg.value == ZYDIS_REGISTER_K0))
            {
                return ZYAN_FALSE;
            }
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    if (user_op->reg.is4 != is4_expected_value)
    {
        return ZYAN_FALSE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if requested operand matches memory operand from instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   user_op     Operand definition from `ZydisEncoderRequest` structure.
 * @param   def_op      Decoder's operand definition from current instruction definition.
 *
 * @return  True if operands match, false otherwise.
 */
static ZyanBool ZydisIsMemoryOperandCompatible(ZydisEncoderInstructionMatch *match,
    const ZydisEncoderOperand *user_op, const ZydisOperandDefinition *def_op)
{
    switch (def_op->type)
    {
    case ZYDIS_SEMANTIC_OPTYPE_MEM:
    case ZYDIS_SEMANTIC_OPTYPE_AGEN:
    case ZYDIS_SEMANTIC_OPTYPE_MIB:
    case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBX:
    case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBY:
    case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBZ:
    {
        if ((def_op->type == ZYDIS_SEMANTIC_OPTYPE_MIB) &&
            (user_op->mem.scale != 0))
        {
            return ZYAN_FALSE;
        }
        ZyanI64 displacement = user_op->mem.displacement;
        ZyanU8 disp_size = 0;
        if (displacement)
        {
            disp_size = ZydisGetSignedImmSize(displacement);
            if (disp_size > 32)
            {
                return ZYAN_FALSE;
            }
            if (ZydisGetMachineModeWidth(match->request->machine_mode) == 16)
            {
                if ((ZyanI16)displacement == 0)
                {
                    disp_size = 0;
                }
                else
                {
                    disp_size = ZydisGetSignedImmSize((ZyanI16)displacement);
                }
            }

            match->cd8_scale = ZydisGetCompDispScale(match);
            if (match->cd8_scale)
            {
                const ZyanI64 mask = (1 << match->cd8_scale) - 1;
                if (!(displacement & mask))
                {
                    disp_size = ZydisGetSignedImmSize(displacement >> match->cd8_scale);
                }
                else if (disp_size == 8)
                {
                    disp_size = 16;
                }
            }
        }

        if (def_op->type != ZYDIS_SEMANTIC_OPTYPE_AGEN)
        {
            if (match->eosz != 0)
            {
                const ZyanU8 eosz_index = match->eosz >> 5;
                if (def_op->size[eosz_index] != user_op->mem.size)
                {
                    return ZYAN_FALSE;
                }
            }
            else if ((match->definition->vector_length != ZYDIS_VECTOR_LENGTH_INVALID) ||
                     (match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX))
            {
                ZyanU8 eosz_index = ZydisGetMachineModeWidth(match->request->machine_mode) >> 5;
                if (match->eosz64_forbidden && (eosz_index == 2))
                {
                    eosz_index = 1;
                }
                ZyanU16 allowed_mem_size = def_op->size[eosz_index];
                if ((!allowed_mem_size) &&
                    (match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_VEX))
                {
                    ZYAN_ASSERT((match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) ||
                                (match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX));
                    switch (match->definition->vector_length)
                    {
                    case ZYDIS_VECTOR_LENGTH_128:
                        allowed_mem_size = 16;
                        break;
                    case ZYDIS_VECTOR_LENGTH_256:
                        allowed_mem_size = 32;
                        break;
                    case ZYDIS_VECTOR_LENGTH_INVALID:
                        ZYAN_ASSERT(match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX);
                        ZYAN_FALLTHROUGH;
                    case ZYDIS_VECTOR_LENGTH_512:
                        allowed_mem_size = 64;
                        break;
                    default:
                        ZYAN_UNREACHABLE;
                    }
                    if (match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX)
                    {
                        const ZydisInstructionDefinitionEVEX *evex_def =
                            (const ZydisInstructionDefinitionEVEX *)match->base_definition;
                        static const ZyanU8 element_sizes[ZYDIS_IELEMENT_SIZE_MAX_VALUE + 1] =
                        {
                              0, 1, 2, 4, 8, 16
                        };
                        ZYAN_ASSERT(evex_def->element_size < ZYAN_ARRAY_LENGTH(element_sizes));
                        const ZyanU8 element_size = element_sizes[evex_def->element_size];
                        if (match->request->evex.broadcast || evex_def->broadcast)
                        {
                            allowed_mem_size = element_size;
                        }
                        else
                        {
                            switch (evex_def->tuple_type)
                            {
                            case ZYDIS_TUPLETYPE_FV:
                                break;
                            case ZYDIS_TUPLETYPE_HV:
                                allowed_mem_size /= 2;
                                break;
                            case ZYDIS_TUPLETYPE_QUARTER:
                                allowed_mem_size /= 4;
                                break;
                            default:
                                ZYAN_UNREACHABLE;
                            }
                        }
                    }
                    else
                    {
                        const ZydisInstructionDefinitionMVEX *mvex_def =
                            (const ZydisInstructionDefinitionMVEX *)match->base_definition;
                        ZyanU16 element_size;
                        switch (match->request->mvex.conversion)
                        {
                        case ZYDIS_CONVERSION_MODE_INVALID:

                            switch (mvex_def->functionality)
                            {
                            case ZYDIS_MVEX_FUNC_SF_32:
                            case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
                            case ZYDIS_MVEX_FUNC_UF_32:
                            case ZYDIS_MVEX_FUNC_DF_32:
                            case ZYDIS_MVEX_FUNC_SI_32:
                            case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
                            case ZYDIS_MVEX_FUNC_UI_32:
                            case ZYDIS_MVEX_FUNC_DI_32:
                                allowed_mem_size = 64;
                                element_size = 4;
                                break;
                            case ZYDIS_MVEX_FUNC_SF_64:
                            case ZYDIS_MVEX_FUNC_UF_64:
                            case ZYDIS_MVEX_FUNC_DF_64:
                            case ZYDIS_MVEX_FUNC_SI_64:
                            case ZYDIS_MVEX_FUNC_UI_64:
                            case ZYDIS_MVEX_FUNC_DI_64:
                                allowed_mem_size = 64;
                                element_size = 8;
                                break;
                            case ZYDIS_MVEX_FUNC_SF_32_BCST:
                            case ZYDIS_MVEX_FUNC_SI_32_BCST:
                                allowed_mem_size = 32;
                                element_size = 4;
                                break;
                            default:
                                ZYAN_UNREACHABLE;
                            }
                            break;
                        case ZYDIS_CONVERSION_MODE_FLOAT16:
                        case ZYDIS_CONVERSION_MODE_SINT16:
                        case ZYDIS_CONVERSION_MODE_UINT16:
                            allowed_mem_size = 32;
                            element_size = 2;
                            break;
                        case ZYDIS_CONVERSION_MODE_SINT8:
                        case ZYDIS_CONVERSION_MODE_UINT8:
                            allowed_mem_size = 16;
                            element_size = 1;
                            break;
                        default:
                            ZYAN_UNREACHABLE;
                        }
                        ZYAN_ASSERT(!mvex_def->broadcast || !match->request->mvex.broadcast);
                        switch (mvex_def->broadcast)
                        {
                        case ZYDIS_MVEX_STATIC_BROADCAST_NONE:
                            break;
                        case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_8:
                        case ZYDIS_MVEX_STATIC_BROADCAST_1_TO_16:
                            allowed_mem_size = element_size;
                            break;
                        case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_8:
                        case ZYDIS_MVEX_STATIC_BROADCAST_4_TO_16:
                            allowed_mem_size = element_size * 4;
                            break;
                        default:
                            ZYAN_UNREACHABLE;
                        }
                        switch (match->request->mvex.broadcast)
                        {
                        case ZYDIS_BROADCAST_MODE_INVALID:
                            break;
                        case ZYDIS_BROADCAST_MODE_1_TO_8:
                        case ZYDIS_BROADCAST_MODE_1_TO_16:
                            allowed_mem_size = element_size;
                            break;
                        case ZYDIS_BROADCAST_MODE_4_TO_8:
                        case ZYDIS_BROADCAST_MODE_4_TO_16:
                            allowed_mem_size = element_size * 4;
                            break;
                        default:
                            ZYAN_UNREACHABLE;
                        }
                    }
                }
                if (user_op->mem.size != allowed_mem_size)
                {
                    return ZYAN_FALSE;
                }
            }
            else if (match->definition->rex_w)
            {
                match->eosz = 64;
            }
            else if (match->definition->vector_length == ZYDIS_VECTOR_LENGTH_INVALID)
            {
                match->eosz = ZydisGetOperandSizeFromElementSize(match, def_op->size,
                    user_op->mem.size, ZYAN_TRUE);
                if (match->eosz == 0)
                {
                    return ZYAN_FALSE;
                }
            }
            else
            {
                ZYAN_UNREACHABLE;
            }
        }
        else
        {
            if (match->easz != 0)
            {
                if (match->easz != user_op->mem.size)
                {
                    return ZYAN_FALSE;
                }
            }
            else
            {
                switch (user_op->mem.size)
                {
                case 2:
                case 4:
                case 8:
                    match->easz = (ZyanU8)user_op->mem.size << 3;
                    break;
                default:
                    return ZYAN_FALSE;
                }
            }
        }

        ZydisRegisterClass vsib_index_class = ZYDIS_REGCLASS_INVALID;
        ZyanBool is_vsib = ZYAN_TRUE;
        switch (def_op->type)
        {
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBX:
            vsib_index_class = ZYDIS_REGCLASS_XMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBY:
            vsib_index_class = ZYDIS_REGCLASS_YMM;
            break;
        case ZYDIS_SEMANTIC_OPTYPE_MEM_VSIBZ:
            vsib_index_class = ZYDIS_REGCLASS_ZMM;
            break;
        default:
            is_vsib = ZYAN_FALSE;
            break;
        }
        const ZyanBool is_rip_relative = (user_op->mem.base == ZYDIS_REGISTER_RIP) ||
                                         (user_op->mem.base == ZYDIS_REGISTER_EIP);
        if (is_rip_relative)
        {
            const ZyanBool no_rip_rel = ZYDIS_OPDEF_GET_MEM_HIGH_BIT(match->base_definition->op_rm);
            if (no_rip_rel || ((match->definition->modrm & 7) == 4))
            {
                return ZYAN_FALSE;
            }
        }
        const ZydisRegisterClass reg_base_class = ZydisRegisterGetClass(user_op->mem.base);
        if ((reg_base_class == ZYDIS_REGCLASS_INVALID) &&
            (user_op->mem.base != ZYDIS_REGISTER_NONE))
        {
            return ZYAN_FALSE;
        }
        const ZydisRegisterClass reg_index_class = ZydisRegisterGetClass(user_op->mem.index);
        if ((reg_index_class == ZYDIS_REGCLASS_INVALID) &&
            (user_op->mem.index != ZYDIS_REGISTER_NONE))
        {
            return ZYAN_FALSE;
        }
        if (is_vsib)
        {
            const ZyanU8 mode_width = ZydisGetMachineModeWidth(match->request->machine_mode);
            const ZyanI8 reg_index_id = ZydisRegisterGetId(user_op->mem.index);
            if (((match->request->machine_mode != ZYDIS_MACHINE_MODE_LONG_64) ||
                 (reg_base_class != ZYDIS_REGCLASS_GPR64)) &&
                 (reg_base_class != ZYDIS_REGCLASS_GPR32) &&
                 (reg_base_class != ZYDIS_REGCLASS_INVALID))
            {
                return ZYAN_FALSE;
            }
            if ((reg_base_class == ZYDIS_REGCLASS_GPR32) &&
                (mode_width != 64) &&
                (ZydisRegisterGetId(user_op->mem.base) > 7))
            {
                return ZYAN_FALSE;
            }
            ZyanU8 max_reg_id = 7;
            if (mode_width == 64)
            {
                max_reg_id = match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_VEX ?
                    31 : 15;
            }
            if ((reg_index_class != vsib_index_class) ||
                (reg_index_id > max_reg_id))
            {
                return ZYAN_FALSE;
            }
        }
        else
        {
            if (!ZydisIsValidAddressingClass(match, reg_base_class, user_op->mem.base))
            {
                if (!is_rip_relative || match->request->machine_mode != ZYDIS_MACHINE_MODE_LONG_64)
                {
                    return ZYAN_FALSE;
                }
            }
            if (!ZydisIsValidAddressingClass(match, reg_index_class, user_op->mem.index))
            {
                return ZYAN_FALSE;
            }
            if (reg_base_class != ZYDIS_REGCLASS_INVALID &&
                reg_index_class != ZYDIS_REGCLASS_INVALID &&
                reg_base_class != reg_index_class)
            {
                return ZYAN_FALSE;
            }
            if (user_op->mem.index == ZYDIS_REGISTER_ESP ||
                user_op->mem.index == ZYDIS_REGISTER_RSP)
            {
                return ZYAN_FALSE;
            }
        }
        if (reg_index_class != ZYDIS_REGCLASS_INVALID &&
            user_op->mem.scale == 0 &&
            def_op->type != ZYDIS_SEMANTIC_OPTYPE_MIB)
        {
            return ZYAN_FALSE;
        }
        if (reg_index_class == ZYDIS_REGCLASS_INVALID &&
            user_op->mem.scale != 0)
        {
            return ZYAN_FALSE;
        }
        ZyanU8 candidate_easz = 0;
        ZyanBool disp_only = ZYAN_FALSE;
        if (reg_base_class != ZYDIS_REGCLASS_INVALID)
        {
            if (is_rip_relative)
            {
                candidate_easz = user_op->mem.base == ZYDIS_REGISTER_RIP ? 64 : 32;
            }
            else
            {
                candidate_easz = (ZyanU8)ZydisRegisterClassGetWidth(match->request->machine_mode,
                    reg_base_class);
            }
        }
        else if (reg_index_class != ZYDIS_REGCLASS_INVALID)
        {
            if (is_vsib)
            {
                candidate_easz = ZydisGetMachineModeWidth(match->request->machine_mode);
            }
            else
            {
                candidate_easz = (ZyanU8)ZydisRegisterClassGetWidth(match->request->machine_mode,
                    reg_index_class);
            }
        }
        else
        {
            ZyanU8 min_disp_size = match->easz ? match->easz : 16;
            if (((min_disp_size == 16) && !(match->definition->address_sizes & ZYDIS_WIDTH_16)) ||
                 (min_disp_size == 64))
            {
                min_disp_size = 32;
            }
            if (ZydisGetUnsignedImmSize(displacement) == 16)
            {
                disp_size = 16;
            }
            if (disp_size < min_disp_size)
            {
                disp_size = min_disp_size;
            }
            if (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
            {
                candidate_easz = match->easz == 32 ? 32 : 64;
            }
            else
            {
                candidate_easz = disp_size;
            }
            disp_only = ZYAN_TRUE;
        }
        if (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
        {
            if (is_rip_relative && reg_index_class != ZYDIS_REGCLASS_INVALID)
            {
                return ZYAN_FALSE;
            }
        }
        else
        {
            if (candidate_easz == 16 && !disp_only)
            {
                if (disp_size > 16)
                {
                    return ZYAN_FALSE;
                }
                const ZyanI8 rm16 = ZydisGetRm16(user_op->mem.base, user_op->mem.index);
                if (rm16 == -1)
                {
                    return ZYAN_FALSE;
                }
                const ZyanU8 allowed_scale = rm16 < 4 ? 1 : 0;
                if (user_op->mem.scale != allowed_scale)
                {
                    return ZYAN_FALSE;
                }
            }
        }
        if (match->easz != 0)
        {
            if (match->easz != candidate_easz)
            {
                return ZYAN_FALSE;
            }
        }
        else
        {
            match->easz = candidate_easz;
        }
        if ((match->base_definition->address_size_map == ZYDIS_ADSIZE_MAP_IGNORED) &&
            (match->easz != ZydisGetMachineModeWidth(match->request->machine_mode)))
        {
            return ZYAN_FALSE;
        }
        match->disp_size = disp_size;
        break;
    }
    case ZYDIS_SEMANTIC_OPTYPE_MOFFS:
        if (user_op->mem.base != ZYDIS_REGISTER_NONE ||
            user_op->mem.index != ZYDIS_REGISTER_NONE ||
            user_op->mem.scale != 0)
        {
            return ZYAN_FALSE;
        }
        if (match->eosz != 0)
        {
            const ZyanU8 eosz_index = match->eosz >> 5;
            if (def_op->size[eosz_index] != user_op->mem.size)
            {
                return ZYAN_FALSE;
            }
        }
        else
        {
            match->eosz = ZydisGetOperandSizeFromElementSize(match, def_op->size,
                user_op->mem.size, ZYAN_TRUE);
            if (match->eosz == 0)
            {
                return ZYAN_FALSE;
            }
        }
        match->disp_size = ZydisGetEffectiveImmSize(match, user_op->mem.displacement, def_op);
        if (match->disp_size == 0)
        {
            return ZYAN_FALSE;
        }
        // This is not a standard rejection. It's a special case for `mov` instructions (only ones
        // to use `moffs` operands). Size of `moffs` is tied to address size attribute, so its
        // signedness doesn't matter. However if displacement can be represented as a signed
        // integer of smaller size we reject `moffs` variant because it's guaranteed that better
        // alternative exists (in terms of size).
        ZyanU8 alternative_size = ZydisGetSignedImmSize(user_op->mem.displacement);
        const ZyanU8 min_disp_size =
            (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) ? 32 : 16;
        if (alternative_size < min_disp_size)
        {
            alternative_size = min_disp_size;
        }
        if (alternative_size < match->disp_size)
        {
            return ZYAN_FALSE;
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if requested operand matches pointer operand from instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   user_op     Operand definition from `ZydisEncoderRequest` structure.
 *
 * @return  True if operands match, false otherwise.
 */
static ZyanBool ZydisIsPointerOperandCompatible(ZydisEncoderInstructionMatch *match,
    const ZydisEncoderOperand *user_op)
{
    ZYAN_ASSERT(match->eosz == 0);
    ZYAN_ASSERT(match->request->machine_mode != ZYDIS_MACHINE_MODE_LONG_64);
    ZYAN_ASSERT((match->request->branch_type == ZYDIS_BRANCH_TYPE_NONE) ||
                (match->request->branch_type == ZYDIS_BRANCH_TYPE_FAR));
    const ZyanU8 min_disp_size = ZydisGetUnsignedImmSize(user_op->ptr.offset);
    const ZyanU8 desired_disp_size = (match->request->branch_width == ZYDIS_BRANCH_WIDTH_NONE)
        ? ZydisGetMachineModeWidth(match->request->machine_mode)
        : (4 << match->request->branch_width);
    if (min_disp_size > desired_disp_size)
    {
        return ZYAN_FALSE;
    }
    match->eosz = match->disp_size = desired_disp_size;
    match->imm_size = 16;
    return ZYAN_TRUE;
}

/**
 * Checks if requested operand matches immediate operand from instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   user_op     Operand definition from `ZydisEncoderRequest` structure.
 * @param   def_op      Decoder's operand definition from current instruction definition.
 *
 * @return  True if operands match, false otherwise.
 */
static ZyanBool ZydisIsImmediateOperandCompabile(ZydisEncoderInstructionMatch *match,
    const ZydisEncoderOperand *user_op, const ZydisOperandDefinition *def_op)
{
    switch (def_op->type)
    {
    case ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_IMM1:
        if (user_op->imm.u != 1)
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_SEMANTIC_OPTYPE_IMM:
    case ZYDIS_SEMANTIC_OPTYPE_REL:
    {
        const ZyanU8 imm_size = ZydisGetEffectiveImmSize(match, user_op->imm.s, def_op);
        if (def_op->op.encoding != ZYDIS_OPERAND_ENCODING_IS4)
        {
            if (imm_size == 0)
            {
                return ZYAN_FALSE;
            }
            if (match->imm_size)
            {
                ZYAN_ASSERT(match->disp_size == 0);
                match->disp_size = match->imm_size;
            }
        }
        else
        {
            ZYAN_ASSERT(match->imm_size == 0);
            if (imm_size != 8)
            {
                return ZYAN_FALSE;
            }
        }
        match->imm_size = imm_size;
        match->has_rel_operand = (def_op->type == ZYDIS_SEMANTIC_OPTYPE_REL);
        break;
    }
    default:
        ZYAN_UNREACHABLE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if requested boardcast mode is compatible with instruction definition.
 *
 * @param   evex_def       Definition for `EVEX`-encoded instruction.
 * @param   vector_length  Vector length.
 * @param   broadcast      Requested broadcast mode.
 *
 * @return  True if broadcast mode is compatible, false otherwise.
 */
static ZyanBool ZydisIsBroadcastModeCompatible(const ZydisInstructionDefinitionEVEX *evex_def,
    ZydisVectorLength vector_length, ZydisBroadcastMode broadcast)
{
    if (broadcast == ZYDIS_BROADCAST_MODE_INVALID)
    {
        return ZYAN_TRUE;
    }

    ZyanU8 vector_size = 0;
    ZYAN_ASSERT(vector_length != ZYDIS_VECTOR_LENGTH_INVALID);
    switch (vector_length)
    {
    case ZYDIS_VECTOR_LENGTH_128:
        vector_size = 16;
        break;
    case ZYDIS_VECTOR_LENGTH_256:
        vector_size = 32;
        break;
    case ZYDIS_VECTOR_LENGTH_512:
        vector_size = 64;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    switch (evex_def->tuple_type)
    {
    case ZYDIS_TUPLETYPE_FV:
        break;
    case ZYDIS_TUPLETYPE_HV:
        vector_size /= 2;
        break;
    case ZYDIS_TUPLETYPE_QUARTER:
        vector_size /= 4;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    ZyanU8 element_size;
    switch (evex_def->element_size)
    {
    case ZYDIS_IELEMENT_SIZE_16:
        element_size = 2;
        break;
    case ZYDIS_IELEMENT_SIZE_32:
        element_size = 4;
        break;
    case ZYDIS_IELEMENT_SIZE_64:
        element_size = 8;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    ZydisBroadcastMode allowed_mode;
    const ZyanU8 element_count = vector_size / element_size;
    switch (element_count)
    {
    case 2:
        allowed_mode = ZYDIS_BROADCAST_MODE_1_TO_2;
        break;
    case 4:
        allowed_mode = ZYDIS_BROADCAST_MODE_1_TO_4;
        break;
    case 8:
        allowed_mode = ZYDIS_BROADCAST_MODE_1_TO_8;
        break;
    case 16:
        allowed_mode = ZYDIS_BROADCAST_MODE_1_TO_16;
        break;
    case 32:
        allowed_mode = ZYDIS_BROADCAST_MODE_1_TO_32;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    if (broadcast != allowed_mode)
    {
        return ZYAN_FALSE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if requested `EVEX`-specific features are compatible with instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   request     A pointer to `ZydisEncoderRequest` struct.
 *
 * @return  True if features are compatible, false otherwise.
 */
static ZyanBool ZydisAreEvexFeaturesCompatible(const ZydisEncoderInstructionMatch *match,
    const ZydisEncoderRequest *request)
{
    if (match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_EVEX)
    {
        return ZYAN_TRUE;
    }

    const ZydisInstructionDefinitionEVEX *evex_def =
        (const ZydisInstructionDefinitionEVEX *)match->base_definition;
    if ((!evex_def->accepts_zero_mask) &&
        (evex_def->mask_override != ZYDIS_MASK_OVERRIDE_ZEROING) &&
        (request->evex.zeroing_mask))
    {
        return ZYAN_FALSE;
    }

    switch (evex_def->functionality)
    {
    case ZYDIS_EVEX_FUNC_INVALID:
        if ((request->evex.sae) ||
            (request->evex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->evex.rounding != ZYDIS_ROUNDING_MODE_INVALID))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_EVEX_FUNC_BC:
        if ((request->evex.sae) ||
            (request->evex.rounding != ZYDIS_ROUNDING_MODE_INVALID))
        {
            return ZYAN_FALSE;
        }
        if (!ZydisIsBroadcastModeCompatible(evex_def, match->definition->vector_length,
            request->evex.broadcast))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_EVEX_FUNC_RC:
        if (request->evex.broadcast != ZYDIS_BROADCAST_MODE_INVALID)
        {
            return ZYAN_FALSE;
        }
        if (request->evex.rounding == ZYDIS_ROUNDING_MODE_INVALID)
        {
            if (request->evex.sae)
            {
                return ZYAN_FALSE;
            }
        }
        else
        {
            if (!request->evex.sae)
            {
                return ZYAN_FALSE;
            }
        }
        break;
    case ZYDIS_EVEX_FUNC_SAE:
        if ((request->evex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->evex.rounding != ZYDIS_ROUNDING_MODE_INVALID))
        {
            return ZYAN_FALSE;
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if requested `MVEX`-specific features are compatible with instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   request     A pointer to `ZydisEncoderRequest` struct.
 *
 * @return  True if features are compatible, false otherwise.
 */
static ZyanBool ZydisAreMvexFeaturesCompatible(const ZydisEncoderInstructionMatch *match,
    const ZydisEncoderRequest *request)
{
    if (match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_MVEX)
    {
        return ZYAN_TRUE;
    }
    if (((match->definition->modrm >> 6) == 3) &&
        (request->mvex.eviction_hint))
    {
        return ZYAN_FALSE;
    }

    const ZydisInstructionDefinitionMVEX *mvex_def =
        (const ZydisInstructionDefinitionMVEX *)match->base_definition;
    switch (mvex_def->functionality)
    {
    case ZYDIS_MVEX_FUNC_IGNORED:
    case ZYDIS_MVEX_FUNC_INVALID:
    case ZYDIS_MVEX_FUNC_F_32:
    case ZYDIS_MVEX_FUNC_I_32:
    case ZYDIS_MVEX_FUNC_F_64:
    case ZYDIS_MVEX_FUNC_I_64:
    case ZYDIS_MVEX_FUNC_UF_64:
    case ZYDIS_MVEX_FUNC_UI_64:
    case ZYDIS_MVEX_FUNC_DF_64:
    case ZYDIS_MVEX_FUNC_DI_64:
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_RC:
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.eviction_hint))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SAE:
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.eviction_hint))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SWIZZLE_32:
    case ZYDIS_MVEX_FUNC_SWIZZLE_64:
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SF_32:
        if ((request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_1_TO_16) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_4_TO_16))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_FLOAT16) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_UINT8) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_UINT16) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_SINT16))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SI_32:
        if ((request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_1_TO_16) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_4_TO_16))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_UINT8) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_SINT8) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_UINT16) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_SINT16))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SF_32_BCST:
    case ZYDIS_MVEX_FUNC_SI_32_BCST:
        if ((request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_1_TO_16) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_4_TO_16))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SF_32_BCST_4TO16:
    case ZYDIS_MVEX_FUNC_SI_32_BCST_4TO16:
        if ((request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_4_TO_16))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_SF_64:
    case ZYDIS_MVEX_FUNC_SI_64:
        if ((request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_1_TO_8) &&
            (request->mvex.broadcast != ZYDIS_BROADCAST_MODE_4_TO_8))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_UF_32:
    case ZYDIS_MVEX_FUNC_DF_32:
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        break;
    case ZYDIS_MVEX_FUNC_UI_32:
    case ZYDIS_MVEX_FUNC_DI_32:
        if ((request->mvex.broadcast != ZYDIS_BROADCAST_MODE_INVALID) ||
            (request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID) ||
            (request->mvex.swizzle != ZYDIS_SWIZZLE_MODE_INVALID) ||
            (request->mvex.sae))
        {
            return ZYAN_FALSE;
        }
        if ((request->mvex.conversion != ZYDIS_CONVERSION_MODE_INVALID) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_UINT8) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_SINT8) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_UINT16) &&
            (request->mvex.conversion != ZYDIS_CONVERSION_MODE_SINT16))
        {
            return ZYAN_FALSE;
        }
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if operands specified in encoder request satisfy additional constraints mandated by
 * matched instruction definition.
 *
 * @param   match   A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  True if operands passed the checks, false otherwise.
 */
static ZyanBool ZydisCheckConstraints(const ZydisEncoderInstructionMatch *match)
{
    const ZydisEncoderOperand *operands = match->request->operands;
    ZyanBool is_gather = ZYAN_FALSE;
    switch (match->definition->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    {
        const ZydisInstructionDefinitionVEX *vex_def =
            (const ZydisInstructionDefinitionVEX *)match->base_definition;
        if (vex_def->is_gather)
        {
            ZYAN_ASSERT(match->request->operand_count == 3);
            ZYAN_ASSERT(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
            ZYAN_ASSERT(operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY);
            ZYAN_ASSERT(operands[2].type == ZYDIS_OPERAND_TYPE_REGISTER);
            const ZyanI8 dest = ZydisRegisterGetId(operands[0].reg.value);
            const ZyanI8 index = ZydisRegisterGetId(operands[1].mem.index);
            const ZyanI8 mask = ZydisRegisterGetId(operands[2].reg.value);
            // If any pair of the index, mask, or destination registers are the same, the
            // instruction results a UD fault.
            if ((dest == index) || (dest == mask) || (index == mask))
            {
                return ZYAN_FALSE;
            }
        }

        if (vex_def->no_source_source_match)
        {
            ZYAN_ASSERT(match->request->operand_count == 3);
            ZYAN_ASSERT(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
            ZYAN_ASSERT(operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER);
            ZYAN_ASSERT(operands[2].type == ZYDIS_OPERAND_TYPE_REGISTER);
            const ZydisRegister dest = operands[0].reg.value;
            const ZydisRegister source1 = operands[1].reg.value;
            const ZydisRegister source2 = operands[2].reg.value;
            // AMX-E4: #UD if srcdest == src1 OR src1 == src2 OR srcdest == src2.
            if ((dest == source1) || (source1 == source2) || (dest == source2))
            {
                return ZYAN_FALSE;
            }
        }

        return ZYAN_TRUE;
    }
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    {
        const ZydisInstructionDefinitionEVEX *evex_def =
            (const ZydisInstructionDefinitionEVEX *)match->base_definition;
        is_gather = evex_def->is_gather;
        if (evex_def->no_source_dest_match)
        {
            ZYAN_ASSERT(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
            ZYAN_ASSERT(operands[2].type == ZYDIS_OPERAND_TYPE_REGISTER);
            ZYAN_ASSERT((operands[3].type == ZYDIS_OPERAND_TYPE_REGISTER) ||
                        (operands[3].type == ZYDIS_OPERAND_TYPE_MEMORY));
            const ZydisRegister dest = operands[0].reg.value;
            const ZydisRegister source1 = operands[2].reg.value;
            const ZydisRegister source2 = (operands[3].type == ZYDIS_OPERAND_TYPE_REGISTER)
                ? operands[3].reg.value
                : ZYDIS_REGISTER_NONE;

            if ((dest == source1) || (dest == source2))
            {
                return ZYAN_FALSE;
            }
        }
        break;
    }
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
    {
        const ZydisInstructionDefinitionMVEX *mvex_def =
            (const ZydisInstructionDefinitionMVEX *)match->base_definition;
        is_gather = mvex_def->is_gather;
        break;
    }
    default:
        return ZYAN_TRUE;
    }

    if ((is_gather) && (operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER))
    {
        ZYAN_ASSERT(match->request->operand_count == 3);
        ZYAN_ASSERT(operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER);
        ZYAN_ASSERT(operands[2].type == ZYDIS_OPERAND_TYPE_MEMORY);
        const ZyanI8 dest = ZydisRegisterGetId(operands[0].reg.value);
        const ZyanI8 index = ZydisRegisterGetId(operands[2].mem.index);
        // EVEX: The instruction will #UD fault if the destination vector zmm1 is the same as
        // index vector VINDEX.
        // MVEX: The KNC GATHER instructions forbid using the same vector register for destination
        // and for the index. (https://github.com/intelxed/xed/issues/281#issuecomment-970074554)
        if (dest == index)
        {
            return ZYAN_FALSE;
        }
    }

    return ZYAN_TRUE;
}

/**
 * Checks if operands and encoding-specific features from `ZydisEncoderRequest` match
 * encoder's instruction definition.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   request     A pointer to `ZydisEncoderRequest` struct.
 *
 * @return  True if definition is compatible, false otherwise.
 */
static ZyanBool ZydisIsDefinitionCompatible(ZydisEncoderInstructionMatch *match,
    const ZydisEncoderRequest *request)
{
    ZYAN_ASSERT(request->operand_count == match->base_definition->operand_count_visible);
    match->operands = ZydisGetOperandDefinitions(match->base_definition);

    if (!ZydisAreEvexFeaturesCompatible(match, request))
    {
        return ZYAN_FALSE;
    }
    if (!ZydisAreMvexFeaturesCompatible(match, request))
    {
        return ZYAN_FALSE;
    }

    for (ZyanU8 i = 0; i < request->operand_count; ++i)
    {
        const ZydisEncoderOperand *user_op = &request->operands[i];
        const ZydisOperandDefinition *def_op = &match->operands[i];
        ZYAN_ASSERT(def_op->visibility != ZYDIS_OPERAND_VISIBILITY_HIDDEN);
        ZyanBool is_compatible = ZYAN_FALSE;
        switch (user_op->type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            is_compatible = ZydisIsRegisterOperandCompatible(match, user_op, def_op);
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            is_compatible = ZydisIsMemoryOperandCompatible(match, user_op, def_op);
            break;
        case ZYDIS_OPERAND_TYPE_POINTER:
            is_compatible = ZydisIsPointerOperandCompatible(match, user_op);
            break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            is_compatible = ZydisIsImmediateOperandCompabile(match, user_op, def_op);
            break;
        default:
            ZYAN_UNREACHABLE;
        }

        if (!is_compatible)
        {
            return ZYAN_FALSE;
        }
    }

    ZyanU8 eosz = 0;
    if (match->base_definition->branch_type != ZYDIS_BRANCH_TYPE_NONE)
    {
        switch (request->branch_width)
        {
        case ZYDIS_BRANCH_WIDTH_NONE:
            break;
        case ZYDIS_BRANCH_WIDTH_8:
            if ((!match->has_rel_operand) ||
                (match->base_definition->branch_type != ZYDIS_BRANCH_TYPE_SHORT))
            {
                return ZYAN_FALSE;
            }
            break;
        case ZYDIS_BRANCH_WIDTH_16:
            eosz = 16;
            break;
        case ZYDIS_BRANCH_WIDTH_32:
            eosz = ((match->has_rel_operand) &&
                    (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
                    (match->base_definition->operand_size_map == ZYDIS_OPSIZE_MAP_FORCE64))
                ? 64
                : 32;
            break;
        case ZYDIS_BRANCH_WIDTH_64:
            if (match->has_rel_operand)
            {
                return ZYAN_FALSE;
            }
            eosz = 64;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
    }
    if (eosz)
    {
        if (match->eosz != 0)
        {
            if (match->eosz != eosz)
            {
                return ZYAN_FALSE;
            }
        }
        else
        {
            match->eosz = eosz;
        }
    }

    if (!ZydisCheckConstraints(match))
    {
        return ZYAN_FALSE;
    }

    return ZYAN_TRUE;
}

/**
 * Checks if requested set of prefixes is compatible with instruction definition.
 *
 * @param   match A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  A zyan status code.
 */
static ZyanBool ZydisArePrefixesCompatible(const ZydisEncoderInstructionMatch *match)
{
    // Early-exit optimization for when no prefixes are requested at all.
    if (!(match->attributes & ZYDIS_ENCODABLE_PREFIXES))
    {
        return ZYAN_TRUE;
    }

    if ((!match->base_definition->accepts_segment) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_SEGMENT))
    {
        return ZYAN_FALSE;
    }
    if (match->definition->encoding != ZYDIS_INSTRUCTION_ENCODING_LEGACY)
    {
        return !(match->attributes & ZYDIS_ENCODABLE_PREFIXES_NO_SEGMENTS);
    }

    const ZydisInstructionDefinitionLEGACY *legacy_def =
        (const ZydisInstructionDefinitionLEGACY *)match->base_definition;
    if ((!legacy_def->accepts_LOCK) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_LOCK))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_REP) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_REP))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_REPEREPZ) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_REPE))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_REPNEREPNZ) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_REPNE))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_BOUND) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_BND))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_XACQUIRE) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_XACQUIRE))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_XRELEASE) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_XRELEASE))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_branch_hints) &&
        (match->attributes & (ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN |
                              ZYDIS_ATTRIB_HAS_BRANCH_TAKEN)))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_NOTRACK) &&
        (match->attributes & ZYDIS_ATTRIB_HAS_NOTRACK))
    {
        return ZYAN_FALSE;
    }
    if ((!legacy_def->accepts_hle_without_lock) &&
        (match->attributes & (ZYDIS_ATTRIB_HAS_XACQUIRE |
                              ZYDIS_ATTRIB_HAS_XRELEASE)) &&
        !(match->attributes & ZYDIS_ATTRIB_HAS_LOCK))
    {
        return ZYAN_FALSE;
    }

    return ZYAN_TRUE;
}

/**
 * Returns operand mask containing information about operand count and types in a compressed form.
 *
 * @param   request     A pointer to `ZydisEncoderRequest` struct.
 *
 * @return  Operand mask.
 */
static ZyanU16 ZydisGetOperandMask(const ZydisEncoderRequest *request)
{
    ZyanU16 operand_mask = request->operand_count;
    ZyanU8 bit_offset = ZYAN_BITS_TO_REPRESENT(ZYDIS_ENCODER_MAX_OPERANDS);
    for (ZyanU8 i = 0; i < request->operand_count; ++i)
    {
        operand_mask |= (request->operands[i].type - ZYDIS_OPERAND_TYPE_REGISTER) << bit_offset;
        bit_offset += ZYAN_BITS_TO_REPRESENT(
            ZYDIS_OPERAND_TYPE_MAX_VALUE - ZYDIS_OPERAND_TYPE_REGISTER);
    }

    return operand_mask;
}

/**
 * Handles optimization opportunities indicated by `swappable` field in instruction definition
 * structure. See `ZydisEncodableInstruction` for more information.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  True if definition has been swapped, false otherwise.
 */
static ZyanBool ZydisHandleSwappableDefinition(ZydisEncoderInstructionMatch *match)
{
    if (!match->definition->swappable)
    {
        return ZYAN_FALSE;
    }

    // Special case for ISA-wide unique conflict between two `mov` variants
    // mov gpr16_32_64(encoding=opcode), imm(encoding=simm16_32_64,scale_factor=osz)
    // mov gpr16_32_64(encoding=modrm_rm), imm(encoding=simm16_32_32,scale_factor=osz)
    if (match->request->mnemonic == ZYDIS_MNEMONIC_MOV)
    {
        const ZyanU8 imm_size = ZydisGetSignedImmSize(match->request->operands[1].imm.s);
        if ((match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
            (match->eosz == 64) &&
            (imm_size < 64))
        {
            return ZYAN_TRUE;
        }
    }

    ZYAN_ASSERT((match->request->operand_count == 2) || (match->request->operand_count == 3));
    const ZyanU8 src_index = (match->request->operand_count == 3) ? 2 : 1;
    const ZyanI8 dest_id = ZydisRegisterGetId(match->request->operands[0].reg.value);
    const ZyanI8 src_id = ZydisRegisterGetId(match->request->operands[src_index].reg.value);
    if ((dest_id <= 7) && (src_id > 7))
    {
        ++match->definition;
        ZydisGetInstructionDefinition(match->definition->encoding,
            match->definition->instruction_reference, &match->base_definition);
        match->operands = ZydisGetOperandDefinitions(match->base_definition);
        return ZYAN_TRUE;
    }

    return ZYAN_FALSE;
}

/**
 * This function attempts to find a matching instruction definition for provided encoder request.
 *
 * @param   request     A pointer to `ZydisEncoderRequest` struct.
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisFindMatchingDefinition(const ZydisEncoderRequest *request,
    ZydisEncoderInstructionMatch *match)
{
    ZYAN_MEMSET(match, 0, sizeof(ZydisEncoderInstructionMatch));
    match->request = request;
    match->attributes = request->prefixes;

    const ZydisEncodableInstruction *definition = ZYAN_NULL;
    const ZyanU8 definition_count = ZydisGetEncodableInstructions(request->mnemonic, &definition);
    ZYAN_ASSERT(definition && definition_count);
    const ZydisWidthFlag mode_width = ZydisGetMachineModeWidth(request->machine_mode) >> 4;
    const ZyanBool is_compat =
        (request->machine_mode == ZYDIS_MACHINE_MODE_LONG_COMPAT_16) ||
        (request->machine_mode == ZYDIS_MACHINE_MODE_LONG_COMPAT_32);
    const ZyanU8 default_asz = ZydisGetAszFromHint(request->address_size_hint);
    const ZyanU8 default_osz = ZydisGetOszFromHint(request->operand_size_hint);
    const ZyanU16 operand_mask = ZydisGetOperandMask(request);

    for (ZyanU8 i = 0; i < definition_count; ++i, ++definition)
    {
        if (definition->operand_mask != operand_mask)
        {
            continue;
        }
        const ZydisInstructionDefinition *base_definition = ZYAN_NULL;
        ZydisGetInstructionDefinition(definition->encoding, definition->instruction_reference,
            &base_definition);
        if (!(definition->modes & mode_width))
        {
            continue;
        }
        if ((request->allowed_encodings != ZYDIS_ENCODABLE_ENCODING_DEFAULT) &&
            !(ZydisGetEncodableEncoding(definition->encoding) & request->allowed_encodings))
        {
            continue;
        }
        if (request->machine_mode == ZYDIS_MACHINE_MODE_REAL_16)
        {
            if (base_definition->requires_protected_mode)
            {
                continue;
            }
            switch (definition->encoding)
            {
            case ZYDIS_INSTRUCTION_ENCODING_XOP:
            case ZYDIS_INSTRUCTION_ENCODING_VEX:
            case ZYDIS_INSTRUCTION_ENCODING_EVEX:
            case ZYDIS_INSTRUCTION_ENCODING_MVEX:
                continue;
            default:
                break;
            }
        }
        else if ((request->machine_mode != ZYDIS_MACHINE_MODE_LONG_64) &&
                 (definition->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX))
        {
            continue;
        }
        if (is_compat && base_definition->no_compat_mode)
        {
            continue;
        }
        if ((request->branch_type != ZYDIS_BRANCH_TYPE_NONE) &&
            (request->branch_type != base_definition->branch_type))
        {
            continue;
        }
        if ((base_definition->branch_type == ZYDIS_BRANCH_TYPE_NONE) &&
            (request->branch_width != ZYDIS_BRANCH_WIDTH_NONE))
        {
            continue;
        }

        match->definition = definition;
        match->base_definition = base_definition;
        match->operands = ZYAN_NULL;
        match->easz = definition->accepts_hint == ZYDIS_SIZE_HINT_ASZ ? default_asz : 0;
        match->eosz = definition->accepts_hint == ZYDIS_SIZE_HINT_OSZ ? default_osz : 0;
        match->disp_size = match->imm_size = match->cd8_scale = 0;
        match->rex_type = ZYDIS_REX_TYPE_UNKNOWN;
        match->eosz64_forbidden = ZYAN_FALSE;
        match->has_rel_operand = ZYAN_FALSE;
        if ((base_definition->operand_size_map != ZYDIS_OPSIZE_MAP_BYTEOP) &&
            (match->eosz == 8))
        {
            continue;
        }
        if (!ZydisArePrefixesCompatible(match))
        {
            continue;
        }
        if (!ZydisIsDefinitionCompatible(match, request))
        {
            continue;
        }
        if (ZydisHandleSwappableDefinition(match))
        {
            if (definition == match->definition)
            {
                continue;
            }
            ++i;
            definition = match->definition;
            base_definition = match->base_definition;
        }

        if (match->easz == 0)
        {
            if (definition->address_sizes & mode_width)
            {
                match->easz = (ZyanU8)(mode_width << 4);
            }
            else if (mode_width == ZYDIS_WIDTH_16)
            {
                match->easz = 32;
            }
            else if (mode_width == ZYDIS_WIDTH_32)
            {
                match->easz = 16;
            }
            else
            {
                match->easz = 32;
            }
            ZYAN_ASSERT(definition->address_sizes & (match->easz >> 4));
        }
        else if (!(definition->address_sizes & (match->easz >> 4)))
        {
            continue;
        }

        if (mode_width == ZYDIS_WIDTH_64)
        {
            if (base_definition->operand_size_map == ZYDIS_OPSIZE_MAP_DEFAULT64)
            {
                if (match->eosz == 0)
                {
                    ZYAN_ASSERT(definition->operand_sizes & (ZYDIS_WIDTH_16 | ZYDIS_WIDTH_64));
                    if (definition->operand_sizes & ZYDIS_WIDTH_64)
                    {
                        match->eosz = 64;
                    }
                    else
                    {
                        match->eosz = 16;
                    }
                }
                else if (match->eosz == 32)
                {
                    continue;
                }
            }
            else if (base_definition->operand_size_map == ZYDIS_OPSIZE_MAP_FORCE64)
            {
                if (match->eosz == 0)
                {
                    match->eosz = 64;
                }
                else if (match->eosz != 64)
                {
                    continue;
                }
            }
        }
        if (match->eosz == 0)
        {
            const ZydisWidthFlag default_width = (mode_width == ZYDIS_WIDTH_64)
                ? ZYDIS_WIDTH_32
                : mode_width;
            if (definition->operand_sizes & default_width)
            {
                match->eosz = (ZyanU8)(default_width << 4);
            }
            else if (definition->operand_sizes & ZYDIS_WIDTH_16)
            {
                match->eosz = 16;
            }
            else if (definition->operand_sizes & ZYDIS_WIDTH_32)
            {
                match->eosz = 32;
            }
            else
            {
                match->eosz = 64;
            }
        }
        else if (match->eosz64_forbidden && match->eosz == 64)
        {
            continue;
        }
        else if (!(definition->operand_sizes & (match->eosz >> 4)))
        {
            continue;
        }

        return ZYAN_STATUS_SUCCESS;
    }

    return ZYDIS_STATUS_IMPOSSIBLE_INSTRUCTION;
}

/**
 * Emits unsigned integer value.
 *
 * @param   data    Value to emit.
 * @param   size    Value size in bytes.
 * @param   buffer  A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitUInt(ZyanU64 data, ZyanU8 size, ZydisEncoderBuffer *buffer)
{
    ZYAN_ASSERT(size == 1 || size == 2 || size == 4 || size == 8);

    const ZyanUSize new_offset = buffer->offset + size;
    if (new_offset > buffer->size)
    {
        return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
    }

    // TODO: fix for big-endian systems
    // The size variable is not passed on purpose to allow the compiler
    // to generate better code with a known size at compile time.
    if (size == 1)
    {
        ZYAN_MEMCPY(buffer->buffer + buffer->offset, &data, 1);
    }
    else if (size == 2)
    {
        ZYAN_MEMCPY(buffer->buffer + buffer->offset, &data, 2);
    }
    else if (size == 4)
    {
        ZYAN_MEMCPY(buffer->buffer + buffer->offset, &data, 4);
    }
    else if (size == 8)
    {
        ZYAN_MEMCPY(buffer->buffer + buffer->offset, &data, 8);
    }
    else
    {
        ZYAN_UNREACHABLE;
    }

    buffer->offset = new_offset;
    return ZYAN_STATUS_SUCCESS;
}

/**
 * Emits a single byte.
 *
 * @param   byte    Value to emit.
 * @param   buffer  A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitByte(ZyanU8 byte, ZydisEncoderBuffer *buffer)
{
    return ZydisEmitUInt(byte, 1, buffer);
}

/**
 * Emits legact prefixes.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitLegacyPrefixes(const ZydisEncoderInstruction *instruction,
    ZydisEncoderBuffer *buffer)
{
    ZyanBool compressed_prefixes = ZYAN_FALSE;
    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        compressed_prefixes = ZYAN_TRUE;
        break;
    default:
        break;
    }

    // Group 1
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_LOCK)
    {
        ZYAN_CHECK(ZydisEmitByte(0xF0, buffer));
    }
    if (!compressed_prefixes)
    {
        if (instruction->attributes & (ZYDIS_ATTRIB_HAS_REPNE |
                                       ZYDIS_ATTRIB_HAS_BND |
                                       ZYDIS_ATTRIB_HAS_XACQUIRE))
        {
            ZYAN_CHECK(ZydisEmitByte(0xF2, buffer));
        }
        if (instruction->attributes & (ZYDIS_ATTRIB_HAS_REP |
                                       ZYDIS_ATTRIB_HAS_REPE |
                                       ZYDIS_ATTRIB_HAS_XRELEASE))
        {
            ZYAN_CHECK(ZydisEmitByte(0xF3, buffer));
        }
    }

    // Group 2
    if (instruction->attributes & (ZYDIS_ATTRIB_HAS_SEGMENT_CS |
                                   ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN))
    {
        ZYAN_CHECK(ZydisEmitByte(0x2E, buffer));
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_SS)
    {
        ZYAN_CHECK(ZydisEmitByte(0x36, buffer));
    }
    if (instruction->attributes & (ZYDIS_ATTRIB_HAS_SEGMENT_DS |
                                   ZYDIS_ATTRIB_HAS_BRANCH_TAKEN))
    {
        ZYAN_CHECK(ZydisEmitByte(0x3E, buffer));
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_ES)
    {
        ZYAN_CHECK(ZydisEmitByte(0x26, buffer));
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS)
    {
        ZYAN_CHECK(ZydisEmitByte(0x64, buffer));
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_GS)
    {
        ZYAN_CHECK(ZydisEmitByte(0x65, buffer));
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_NOTRACK)
    {
        ZYAN_CHECK(ZydisEmitByte(0x3E, buffer));
    }

    // Group 3
    if (!compressed_prefixes)
    {
        if (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE)
        {
            ZYAN_CHECK(ZydisEmitByte(0x66, buffer));
        }
    }

    // Group 4
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_ADDRESSSIZE)
    {
        ZYAN_CHECK(ZydisEmitByte(0x67, buffer));
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Encodes low nibble of `REX` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   high_r          A pointer to `ZyanBool` variable that will be set to true when the
 *                          highest `ModR/M.reg` bit cannot be encoded using `REX` prefix.
 *
 * @return  A zyan status code.
 */
static ZyanU8 ZydisEncodeRexLowNibble(const ZydisEncoderInstruction *instruction, ZyanBool *high_r)
{
    if (high_r)
    {
        *high_r = ZYAN_FALSE;
    }

    ZyanU8 rex = 0;
    if ((instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM) &&
        (instruction->attributes & ZYDIS_ATTRIB_HAS_SIB))
    {
        if (instruction->base & 0x08)
        {
            rex |= 1;
        }
        if (instruction->index & 0x08)
        {
            rex |= 2;
        }
        if (instruction->reg & 0x08)
        {
            rex |= 4;
        }
        if (high_r && (instruction->reg & 0x10))
        {
            *high_r = ZYAN_TRUE;
        }
    }
    else if (instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM)
    {
        if (instruction->rm & 0x08)
        {
            rex |= 1;
        }
        if (instruction->rm & 0x10)
        {
            rex |= 2;
        }
        if (instruction->reg & 0x08)
        {
            rex |= 4;
        }
        if (high_r && (instruction->reg & 0x10))
        {
            *high_r = ZYAN_TRUE;
        }
    }
    else
    {
        if (instruction->rm & 0x08)
        {
            rex |= 1;
        }
    }

    if (instruction->rex_w)
    {
        rex |= 8;
    }

    return rex;
}

/**
 * Emits `REX` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitRex(const ZydisEncoderInstruction *instruction,
    ZydisEncoderBuffer *buffer)
{
    const ZyanU8 rex = ZydisEncodeRexLowNibble(instruction, ZYAN_NULL);
    if (rex || (instruction->attributes & ZYDIS_ATTRIB_HAS_REX))
    {
        ZYAN_CHECK(ZydisEmitByte(0x40 | rex, buffer));
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Encodes common parts of `VEX` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   mmmmm           A pointer to `ZyanU8` variable that will receive `VEX.mmmmm`
 * @param   pp              A pointer to `ZyanU8` variable that will receive `VEX.pp`
 * @param   vvvv            A pointer to `ZyanU8` variable that will receive `VEX.vvvv`
 * @param   rex             A pointer to `ZyanU8` variable that will receive 'REX`
 * @param   high_r          A pointer to `ZyanBool` variable that will be set to true when the
 *                          highest `ModR/M.reg` bit cannot be encoded using `REX` prefix.
 */
static void ZydisEncodeVexCommons(ZydisEncoderInstruction *instruction, ZyanU8 *mmmmm, ZyanU8 *pp,
    ZyanU8 *vvvv, ZyanU8 *rex, ZyanBool *high_r)
{
    switch (instruction->opcode_map)
    {
    case ZYDIS_OPCODE_MAP_DEFAULT:
    case ZYDIS_OPCODE_MAP_0F:
    case ZYDIS_OPCODE_MAP_0F38:
    case ZYDIS_OPCODE_MAP_0F3A:
    case ZYDIS_OPCODE_MAP_MAP5:
    case ZYDIS_OPCODE_MAP_MAP6:
        *mmmmm = (ZyanU8)instruction->opcode_map;
        break;
    case ZYDIS_OPCODE_MAP_XOP8:
    case ZYDIS_OPCODE_MAP_XOP9:
    case ZYDIS_OPCODE_MAP_XOPA:
        *mmmmm = 8 + ((ZyanU8)instruction->opcode_map - ZYDIS_OPCODE_MAP_XOP8);
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    instruction->opcode_map = ZYDIS_OPCODE_MAP_DEFAULT;

    *pp = 0;
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_OPERANDSIZE)
    {
        *pp = 1;
    }
    else if (instruction->attributes & ZYDIS_ATTRIB_HAS_REP)
    {
        *pp = 2;
    }
    else if (instruction->attributes & ZYDIS_ATTRIB_HAS_REPNE)
    {
        *pp = 3;
    }

    *vvvv = ~instruction->vvvv;
    *rex = ZydisEncodeRexLowNibble(instruction, high_r);
}

/**
 * Emits `XOP` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitXop(ZydisEncoderInstruction *instruction, ZydisEncoderBuffer *buffer)
{
    ZyanU8 mmmmm, pp, vvvv, rex;
    ZydisEncodeVexCommons(instruction, &mmmmm, &pp, &vvvv, &rex, ZYAN_NULL);
    ZYAN_ASSERT(instruction->vector_length <= 1);
    const ZyanU8 b1 = (((~rex) & 0x07) << 5) | mmmmm;
    const ZyanU8 b2 = ((rex & 0x08) << 4) | ((vvvv & 0xF) << 3) | (instruction->vector_length << 2) | pp;
    ZYAN_CHECK(ZydisEmitByte(0x8F, buffer));
    ZYAN_CHECK(ZydisEmitByte(b1, buffer));
    ZYAN_CHECK(ZydisEmitByte(b2, buffer));
    return ZYAN_STATUS_SUCCESS;
}

/**
 * Emits `VEX` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitVex(ZydisEncoderInstruction *instruction, ZydisEncoderBuffer *buffer)
{
    ZyanU8 mmmmm, pp, vvvv, rex;
    ZydisEncodeVexCommons(instruction, &mmmmm, &pp, &vvvv, &rex, ZYAN_NULL);
    ZYAN_ASSERT(instruction->vector_length <= 1);
    if (mmmmm != 1 || (rex & 0x0B))
    {
        const ZyanU8 b1 = (((~rex) & 0x07) << 5) | mmmmm;
        const ZyanU8 b2 = ((rex & 0x08) << 4) |
                          ((vvvv & 0xF) << 3) |
                          (instruction->vector_length << 2) |
                          pp;
        ZYAN_CHECK(ZydisEmitByte(0xC4, buffer));
        ZYAN_CHECK(ZydisEmitByte(b1, buffer));
        ZYAN_CHECK(ZydisEmitByte(b2, buffer));
    }
    else
    {
        const ZyanU8 b1 = (((~rex) & 0x04) << 5) |
                          ((vvvv & 0xF) << 3) |
                          (instruction->vector_length << 2) |
                          pp;
        ZYAN_CHECK(ZydisEmitByte(0xC5, buffer));
        ZYAN_CHECK(ZydisEmitByte(b1, buffer));
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Encodes common parts of `EVEX` prefix.
 *
 * @param   instruction A pointer to `ZydisEncoderInstruction` struct.
 * @param   p0          A pointer to `ZyanU8` variable that will receive 2nd byte of `EVEX` prefix.
 * @param   p1          A pointer to `ZyanU8` variable that will receive 3rd byte of `EVEX` prefix.
 * @param   vvvvv       A pointer to `ZyanU8` variable that will receive `EVEX.vvvvv`.
 */
static void ZydisEncodeEvexCommons(ZydisEncoderInstruction *instruction, ZyanU8 *p0, ZyanU8 *p1,
    ZyanU8 *vvvvv)
{
    ZyanBool high_r;
    ZyanU8 mmmmm, pp, rex;
    ZydisEncodeVexCommons(instruction, &mmmmm, &pp, vvvvv, &rex, &high_r);
    *p0 = (((~rex) & 0x07) << 5) | mmmmm;
    if (!high_r)
    {
        *p0 |= 0x10;
    }
    *p1 = ((rex & 0x08) << 4) | ((*vvvvv & 0x0F) << 3) | 0x04 | pp;
}

/**
 * Emits `EVEX` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitEvex(ZydisEncoderInstruction *instruction, ZydisEncoderBuffer *buffer)
{
    ZyanU8 p0, p1, vvvvv;
    ZydisEncodeEvexCommons(instruction, &p0, &p1, &vvvvv);
    ZyanU8 p2 = (instruction->vector_length << 5) | ((vvvvv & 0x10) >> 1) | instruction->mask;
    if (instruction->zeroing)
    {
        p2 |= 0x80;
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_EVEX_B)
    {
        p2 |= 0x10;
    }
    if (instruction->index & 0x10)
    {
        p2 &= 0xF7;
    }

    ZYAN_CHECK(ZydisEmitByte(0x62, buffer));
    ZYAN_CHECK(ZydisEmitByte(p0, buffer));
    ZYAN_CHECK(ZydisEmitByte(p1, buffer));
    ZYAN_CHECK(ZydisEmitByte(p2, buffer));
    return ZYAN_STATUS_SUCCESS;
}

/**
 * Emits `MVEX` prefix.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitMvex(ZydisEncoderInstruction *instruction, ZydisEncoderBuffer *buffer)
{
    ZyanU8 p0, p1, vvvvv;
    ZydisEncodeEvexCommons(instruction, &p0, &p1, &vvvvv);
    ZyanU8 p2 = (instruction->sss << 4) | ((vvvvv & 0x10) >> 1) | instruction->mask;
    if (instruction->eviction_hint)
    {
        p2 |= 0x80;
    }
    if (instruction->index & 0x10)
    {
        p2 &= 0xF7;
    }

    ZYAN_CHECK(ZydisEmitByte(0x62, buffer));
    ZYAN_CHECK(ZydisEmitByte(p0, buffer));
    ZYAN_CHECK(ZydisEmitByte(p1 & 0xFB, buffer));
    ZYAN_CHECK(ZydisEmitByte(p2, buffer));
    return ZYAN_STATUS_SUCCESS;
}

/**
 * Emits instruction as stream of bytes.
 *
 * @param   instruction     A pointer to `ZydisEncoderInstruction` struct.
 * @param   buffer          A pointer to `ZydisEncoderBuffer` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEmitInstruction(ZydisEncoderInstruction *instruction,
    ZydisEncoderBuffer *buffer)
{
    ZYAN_CHECK(ZydisEmitLegacyPrefixes(instruction, buffer));

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
        ZYAN_CHECK(ZydisEmitRex(instruction, buffer));
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        ZYAN_CHECK(ZydisEmitXop(instruction, buffer));
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        ZYAN_CHECK(ZydisEmitVex(instruction, buffer));
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        ZYAN_CHECK(ZydisEmitEvex(instruction, buffer));
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        ZYAN_CHECK(ZydisEmitMvex(instruction, buffer));
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    switch (instruction->opcode_map)
    {
    case ZYDIS_OPCODE_MAP_DEFAULT:
        break;
    case ZYDIS_OPCODE_MAP_0F:
        ZYAN_CHECK(ZydisEmitByte(0x0F, buffer));
        break;
    case ZYDIS_OPCODE_MAP_0F38:
        ZYAN_CHECK(ZydisEmitByte(0x0F, buffer));
        ZYAN_CHECK(ZydisEmitByte(0x38, buffer));
        break;
    case ZYDIS_OPCODE_MAP_0F3A:
        ZYAN_CHECK(ZydisEmitByte(0x0F, buffer));
        ZYAN_CHECK(ZydisEmitByte(0x3A, buffer));
        break;
    case ZYDIS_OPCODE_MAP_0F0F:
        ZYAN_CHECK(ZydisEmitByte(0x0F, buffer));
        ZYAN_CHECK(ZydisEmitByte(0x0F, buffer));
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    if (instruction->encoding != ZYDIS_INSTRUCTION_ENCODING_3DNOW)
    {
        ZYAN_CHECK(ZydisEmitByte(instruction->opcode, buffer));
    }

    if (instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM)
    {
        const ZyanU8 modrm = (instruction->mod << 6) |
                             ((instruction->reg & 7) << 3) |
                             (instruction->rm & 7);
        ZYAN_CHECK(ZydisEmitByte(modrm, buffer));
    }
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_SIB)
    {
        const ZyanU8 sib = (instruction->scale << 6) |
                           ((instruction->index & 7) << 3) |
                           (instruction->base & 7);
        ZYAN_CHECK(ZydisEmitByte(sib, buffer));
    }
    if (instruction->disp_size)
    {
        ZYAN_CHECK(ZydisEmitUInt(instruction->disp, instruction->disp_size / 8, buffer));
    }
    if (instruction->imm_size)
    {
        ZYAN_CHECK(ZydisEmitUInt(instruction->imm, instruction->imm_size / 8, buffer));
    }
    if (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_3DNOW)
    {
        ZYAN_CHECK(ZydisEmitByte(instruction->opcode, buffer));
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Encodes register operand as fields inside `ZydisEncoderInstruction` structure.
 *
 * @param   user_op     Validated operand definition from `ZydisEncoderRequest` structure.
 * @param   def_op      Decoder's operand definition from instruction definition.
 * @param   instruction A pointer to `ZydisEncoderInstruction` struct.
 */
void ZydisBuildRegisterOperand(const ZydisEncoderOperand *user_op,
    const ZydisOperandDefinition *def_op, ZydisEncoderInstruction *instruction)
{
    if (def_op->type == ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_REG)
    {
        return;
    }

    ZyanU8 reg_id = 0;
    if (ZydisRegisterGetClass(user_op->reg.value) != ZYDIS_REGCLASS_GPR8)
    {
        reg_id = (ZyanU8)ZydisRegisterGetId(user_op->reg.value);
    }
    else
    {
        static const ZyanU8 reg8_lookup[] = {
            0, 1, 2, 3,                     // AL, CL, DL, BL
            4, 5, 6, 7,                     // AH, CH, DH, BH
            4, 5, 6, 7,                     // SPL, BPL, SIL, DIL
            8, 9, 10, 11, 12, 13, 14, 15,   // R8B-R15B
        };
        ZYAN_ASSERT(
            ((ZyanUSize)user_op->reg.value - ZYDIS_REGISTER_AL) < ZYAN_ARRAY_LENGTH(reg8_lookup));
        reg_id = reg8_lookup[user_op->reg.value - ZYDIS_REGISTER_AL];
        if (user_op->reg.value >= ZYDIS_REGISTER_SPL && user_op->reg.value <= ZYDIS_REGISTER_DIL)
        {
            instruction->attributes |= ZYDIS_ATTRIB_HAS_REX;
        }
    }

    switch (def_op->op.encoding)
    {
    case ZYDIS_OPERAND_ENCODING_MODRM_REG:
        instruction->attributes |= ZYDIS_ATTRIB_HAS_MODRM;
        instruction->reg = reg_id;
        break;
    case ZYDIS_OPERAND_ENCODING_MODRM_RM:
        instruction->attributes |= ZYDIS_ATTRIB_HAS_MODRM;
        instruction->rm = reg_id;
        break;
    case ZYDIS_OPERAND_ENCODING_OPCODE:
        instruction->opcode += reg_id & 7;
        instruction->rm = reg_id;
        break;
    case ZYDIS_OPERAND_ENCODING_NDSNDD:
        instruction->vvvv = reg_id;
        break;
    case ZYDIS_OPERAND_ENCODING_IS4:
        instruction->imm_size = 8;
        instruction->imm = reg_id << 4;
        break;
    case ZYDIS_OPERAND_ENCODING_MASK:
        instruction->mask = reg_id;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Encodes memory operand as fields inside `ZydisEncoderInstruction` structure.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   user_op     Decoder's operand definition from instruction definition.
 * @param   instruction A pointer to `ZydisEncoderInstruction` struct.
 */
static void ZydisBuildMemoryOperand(ZydisEncoderInstructionMatch *match,
    const ZydisEncoderOperand *user_op, ZydisEncoderInstruction *instruction)
{
    instruction->attributes |= ZYDIS_ATTRIB_HAS_MODRM;
    instruction->disp = (ZyanU64)user_op->mem.displacement;
    if (match->easz == 16)
    {
        const ZyanI8 rm = ZydisGetRm16(user_op->mem.base, user_op->mem.index);
        if (rm != -1)
        {
            instruction->rm = (ZyanU8)rm;
            instruction->disp_size = match->disp_size;
            switch (instruction->disp_size)
            {
            case 0:
                if (rm == 6)
                {
                    instruction->disp_size = 8;
                    instruction->mod = 1;
                }
                break;
            case 8:
                instruction->mod = 1;
                break;
            case 16:
                instruction->mod = 2;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
        }
        else
        {
            instruction->rm = 6;
            instruction->disp_size = 16;
        }
        return;
    }

    if (user_op->mem.index == ZYDIS_REGISTER_NONE)
    {
        if (user_op->mem.base == ZYDIS_REGISTER_NONE)
        {
            if (match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
            {
                instruction->rm = 4;
                instruction->attributes |= ZYDIS_ATTRIB_HAS_SIB;
                instruction->base = 5;
                instruction->index = 4;
            }
            else
            {
                instruction->rm = 5;
            }
            instruction->disp_size = 32;
            return;
        }
        else if ((user_op->mem.base == ZYDIS_REGISTER_RIP) ||
                 (user_op->mem.base == ZYDIS_REGISTER_EIP))
        {
            instruction->rm = 5;
            instruction->disp_size = 32;
            return;
        }
    }

    const ZyanU8 reg_base_id = (ZyanU8)ZydisRegisterGetId(user_op->mem.base);
    const ZyanU8 reg_index_id = (ZyanU8)ZydisRegisterGetId(user_op->mem.index);
    instruction->disp_size = match->disp_size;
    switch (instruction->disp_size)
    {
    case 0:
        if (reg_base_id == 5 || reg_base_id == 13)
        {
            instruction->disp_size = 8;
            instruction->disp = 0;
            instruction->mod = 1;
        }
        break;
    case 8:
        instruction->mod = 1;
        break;
    case 16:
        instruction->disp_size = 32;
        ZYAN_FALLTHROUGH;
    case 32:
        instruction->mod = 2;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
    if ((user_op->mem.index == ZYDIS_REGISTER_NONE) &&
        (reg_base_id != 4) &&
        (reg_base_id != 12) &&
        ((match->definition->modrm & 7) != 4))
    {
        instruction->rm = reg_base_id;
        return;
    }
    instruction->rm = 4;
    instruction->attributes |= ZYDIS_ATTRIB_HAS_SIB;
    if (reg_base_id != 0xFF)
    {
        instruction->base = reg_base_id;
    }
    else
    {
        instruction->base = 5;
        instruction->mod = 0;
        instruction->disp_size = 32;
    }
    if (reg_index_id != 0xFF)
    {
        instruction->index = reg_index_id;
    }
    else
    {
        instruction->index = 4;
    }
    switch (user_op->mem.scale)
    {
    case 0:
    case 1:
        break;
    case 2:
        instruction->scale = 1;
        break;
    case 4:
        instruction->scale = 2;
        break;
    case 8:
        instruction->scale = 3;
        break;
    default:
        ZYAN_UNREACHABLE;
    }
}

/**
 * Encodes instruction as emittable `ZydisEncoderInstruction` struct.
 *
 * @param   match       A pointer to `ZydisEncoderInstructionMatch` struct.
 * @param   instruction A pointer to `ZydisEncoderInstruction` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisBuildInstruction(ZydisEncoderInstructionMatch *match,
    ZydisEncoderInstruction *instruction)
{
    ZYAN_MEMSET(instruction, 0, sizeof(ZydisEncoderInstruction));
    instruction->attributes = match->attributes;
    instruction->encoding = match->definition->encoding;
    instruction->opcode_map = match->definition->opcode_map;
    instruction->opcode = match->definition->opcode;
    instruction->rex_w = match->definition->rex_w;
    instruction->mod = (match->definition->modrm >> 6) & 3;
    instruction->reg = (match->definition->modrm >> 3) & 7;
    instruction->rm = match->definition->modrm & 7;
    if (match->definition->modrm)
    {
        instruction->attributes |= ZYDIS_ATTRIB_HAS_MODRM;
    }

    switch (match->definition->vector_length)
    {
    case ZYDIS_VECTOR_LENGTH_INVALID:
    case ZYDIS_VECTOR_LENGTH_128:
        instruction->vector_length = 0;
        break;
    case ZYDIS_VECTOR_LENGTH_256:
        instruction->vector_length = 1;
        break;
    case ZYDIS_VECTOR_LENGTH_512:
        instruction->vector_length = 2;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    if (match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX)
    {
        const ZydisInstructionDefinitionEVEX *evex_def =
            (const ZydisInstructionDefinitionEVEX *)match->base_definition;
        if (evex_def->mask_override != ZYDIS_MASK_OVERRIDE_ZEROING)
        {
            instruction->zeroing = match->request->evex.zeroing_mask;
        }
        if ((match->request->evex.sae) ||
            (match->request->evex.broadcast != ZYDIS_BROADCAST_MODE_INVALID))
        {
            instruction->attributes |= ZYDIS_ATTRIB_HAS_EVEX_B;
        }
        if (match->request->evex.rounding != ZYDIS_ROUNDING_MODE_INVALID)
        {
            instruction->attributes |= ZYDIS_ATTRIB_HAS_EVEX_B;
            switch (match->request->evex.rounding)
            {
            case ZYDIS_ROUNDING_MODE_RN:
                instruction->vector_length = 0;
                break;
            case ZYDIS_ROUNDING_MODE_RD:
                instruction->vector_length = 1;
                break;
            case ZYDIS_ROUNDING_MODE_RU:
                instruction->vector_length = 2;
                break;
            case ZYDIS_ROUNDING_MODE_RZ:
                instruction->vector_length = 3;
                break;
            default:
                ZYAN_UNREACHABLE;
            }
        }
    }
    else if (match->definition->encoding == ZYDIS_INSTRUCTION_ENCODING_MVEX)
    {
        instruction->sss |= ZydisEncodeMvexBroadcastMode(match->request->mvex.broadcast);
        instruction->sss |= ZydisEncodeMvexConversionMode(match->request->mvex.conversion);

        switch (match->request->mvex.rounding)
        {
        case ZYDIS_ROUNDING_MODE_INVALID:
            break;
        case ZYDIS_ROUNDING_MODE_RN:
        case ZYDIS_ROUNDING_MODE_RD:
        case ZYDIS_ROUNDING_MODE_RU:
        case ZYDIS_ROUNDING_MODE_RZ:
            instruction->sss |= match->request->mvex.rounding - ZYDIS_ROUNDING_MODE_RN;
            break;
        default:
            ZYAN_UNREACHABLE;
        }

        switch (match->request->mvex.swizzle)
        {
        case ZYDIS_SWIZZLE_MODE_INVALID:
            break;
        case ZYDIS_SWIZZLE_MODE_DCBA:
        case ZYDIS_SWIZZLE_MODE_CDAB:
        case ZYDIS_SWIZZLE_MODE_BADC:
        case ZYDIS_SWIZZLE_MODE_DACB:
        case ZYDIS_SWIZZLE_MODE_AAAA:
        case ZYDIS_SWIZZLE_MODE_BBBB:
        case ZYDIS_SWIZZLE_MODE_CCCC:
        case ZYDIS_SWIZZLE_MODE_DDDD:
            instruction->sss |= match->request->mvex.swizzle - ZYDIS_SWIZZLE_MODE_DCBA;
            break;
        default:
            ZYAN_UNREACHABLE;
        }

        if ((match->request->mvex.sae) ||
            (match->request->mvex.eviction_hint) ||
            (match->request->mvex.rounding != ZYDIS_ROUNDING_MODE_INVALID))
        {
            instruction->eviction_hint = ZYAN_TRUE;
        }
        if (match->request->mvex.sae)
        {
            instruction->sss |= 4;
        }

        // Following instructions violate general `MVEX.EH` handling rules. In all other cases this
        // bit is used either as eviction hint (memory operands present) or to encode MVEX-specific
        // functionality (register forms). Instructions listed below use `MVEX.EH` to identify
        // different instructions with memory operands and don't treat it as eviction hint.
        switch (match->request->mnemonic)
        {
        case ZYDIS_MNEMONIC_VMOVNRAPD:
        case ZYDIS_MNEMONIC_VMOVNRAPS:
            instruction->eviction_hint = ZYAN_FALSE;
            break;
        case ZYDIS_MNEMONIC_VMOVNRNGOAPD:
        case ZYDIS_MNEMONIC_VMOVNRNGOAPS:
            instruction->eviction_hint = ZYAN_TRUE;
            break;
        default:
            break;
        }
    }

    switch (match->definition->mandatory_prefix)
    {
    case ZYDIS_MANDATORY_PREFIX_NONE:
        break;
    case ZYDIS_MANDATORY_PREFIX_66:
        instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
        break;
    case ZYDIS_MANDATORY_PREFIX_F2:
        instruction->attributes |= ZYDIS_ATTRIB_HAS_REPNE;
        break;
    case ZYDIS_MANDATORY_PREFIX_F3:
        instruction->attributes |= ZYDIS_ATTRIB_HAS_REP;
        break;
    default:
        ZYAN_UNREACHABLE;
    }

    const ZyanU8 mode_width = ZydisGetMachineModeWidth(match->request->machine_mode);
    if (match->easz != mode_width)
    {
        instruction->attributes |= ZYDIS_ATTRIB_HAS_ADDRESSSIZE;
    }
    if ((match->request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
        (match->base_definition->operand_size_map != ZYDIS_OPSIZE_MAP_FORCE64))
    {
        switch (match->eosz)
        {
        case 16:
            instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
            break;
        case 32:
            break;
        case 64:
            instruction->rex_w =
                match->base_definition->operand_size_map != ZYDIS_OPSIZE_MAP_DEFAULT64;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
    }
    else
    {
        if (match->eosz != mode_width)
        {
            instruction->attributes |= ZYDIS_ATTRIB_HAS_OPERANDSIZE;
        }
    }

    for (ZyanU8 i = 0; i < match->request->operand_count; ++i)
    {
        const ZydisEncoderOperand *user_op = &match->request->operands[i];
        const ZydisOperandDefinition *def_op = &match->operands[i];
        switch (user_op->type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            ZydisBuildRegisterOperand(user_op, def_op, instruction);
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            if (def_op->type != ZYDIS_SEMANTIC_OPTYPE_MOFFS)
            {
                ZydisBuildMemoryOperand(match, user_op, instruction);
                if ((match->cd8_scale) &&
                    (instruction->disp_size == 8))
                {
                    instruction->disp >>= match->cd8_scale;
                }
            }
            else
            {
                instruction->disp_size = match->disp_size;
                instruction->disp = (ZyanU64)user_op->mem.displacement;
            }
            break;
        case ZYDIS_OPERAND_TYPE_POINTER:
            instruction->disp_size = match->disp_size;
            instruction->disp = user_op->ptr.offset;
            instruction->imm_size = match->imm_size;
            instruction->imm = user_op->ptr.segment;
            break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            if (def_op->type == ZYDIS_SEMANTIC_OPTYPE_IMPLICIT_IMM1)
            {
                break;
            }
            if (def_op->op.encoding != ZYDIS_OPERAND_ENCODING_IS4)
            {
                if (instruction->imm_size)
                {
                    ZYAN_ASSERT(instruction->disp_size == 0);
                    instruction->disp_size = match->disp_size;
                    instruction->disp = instruction->imm;
                }
                instruction->imm_size = match->imm_size;
                instruction->imm = user_op->imm.u;
            }
            else
            {
                ZYAN_ASSERT(instruction->imm_size == 8);
                instruction->imm |= user_op->imm.u;
            }
            break;
        default:
            ZYAN_UNREACHABLE;
        }
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Performs a set of sanity checks that must be satisfied for every valid encoder request.
 *
 * @param   request A pointer to `ZydisEncoderRequest` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEncoderCheckRequestSanity(const ZydisEncoderRequest *request)
{
    if (((ZyanUSize)request->machine_mode > ZYDIS_MACHINE_MODE_MAX_VALUE) ||
        ((ZyanUSize)request->allowed_encodings > ZYDIS_ENCODABLE_ENCODING_MAX_VALUE) ||
        ((ZyanUSize)request->mnemonic > ZYDIS_MNEMONIC_MAX_VALUE) ||
        ((ZyanUSize)request->branch_type > ZYDIS_BRANCH_TYPE_MAX_VALUE) ||
        ((ZyanUSize)request->branch_width > ZYDIS_BRANCH_WIDTH_MAX_VALUE) ||
        ((ZyanUSize)request->address_size_hint > ZYDIS_ADDRESS_SIZE_HINT_MAX_VALUE) ||
        ((ZyanUSize)request->operand_size_hint > ZYDIS_OPERAND_SIZE_HINT_MAX_VALUE) ||
        ((ZyanUSize)request->evex.broadcast > ZYDIS_BROADCAST_MODE_MAX_VALUE) ||
        ((ZyanUSize)request->evex.rounding > ZYDIS_ROUNDING_MODE_MAX_VALUE) ||
        ((ZyanUSize)request->mvex.broadcast > ZYDIS_BROADCAST_MODE_MAX_VALUE) ||
        ((ZyanUSize)request->mvex.conversion > ZYDIS_CONVERSION_MODE_MAX_VALUE) ||
        ((ZyanUSize)request->mvex.rounding > ZYDIS_ROUNDING_MODE_MAX_VALUE) ||
        ((ZyanUSize)request->mvex.swizzle > ZYDIS_SWIZZLE_MODE_MAX_VALUE) ||
        (request->operand_count > ZYDIS_ENCODER_MAX_OPERANDS) ||
        (request->mnemonic == ZYDIS_MNEMONIC_INVALID) ||
        (request->prefixes & ~ZYDIS_ENCODABLE_PREFIXES))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT)
    {
        if ((request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64) &&
            (request->prefixes & ZYDIS_LEGACY_SEGMENTS))
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }

        ZyanU8 seg_override_count = 0;
        if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT_CS)
        {
            ++seg_override_count;
        }
        if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT_SS)
        {
            ++seg_override_count;
        }
        if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT_DS)
        {
            ++seg_override_count;
        }
        if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT_ES)
        {
            ++seg_override_count;
        }
        if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT_FS)
        {
            ++seg_override_count;
        }
        if (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT_GS)
        {
            ++seg_override_count;
        }
        if (seg_override_count != 1)
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }
    ZyanU8 rep_family_count = 0;
    if (request->prefixes & ZYDIS_ATTRIB_HAS_REP)
    {
        ++rep_family_count;
    }
    if (request->prefixes & ZYDIS_ATTRIB_HAS_REPE)
    {
        ++rep_family_count;
    }
    if (request->prefixes & ZYDIS_ATTRIB_HAS_REPNE)
    {
        ++rep_family_count;
    }
    if (rep_family_count > 1)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((request->prefixes & ZYDIS_ATTRIB_HAS_XACQUIRE) &&
        (request->prefixes & ZYDIS_ATTRIB_HAS_XRELEASE))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((request->prefixes & ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN) &&
        (request->prefixes & ZYDIS_ATTRIB_HAS_BRANCH_TAKEN))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if ((request->prefixes & ZYDIS_ATTRIB_HAS_NOTRACK) &&
        (request->prefixes & ZYDIS_ATTRIB_HAS_SEGMENT))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    static const ZyanBool branch_lookup
        [ZYDIS_BRANCH_WIDTH_MAX_VALUE + 1][ZYDIS_BRANCH_TYPE_MAX_VALUE + 1] =
    {
        /* NONE */ { ZYAN_TRUE,  ZYAN_TRUE,  ZYAN_TRUE,  ZYAN_TRUE  },
        /* 8    */ { ZYAN_TRUE,  ZYAN_TRUE,  ZYAN_FALSE, ZYAN_FALSE },
        /* 16   */ { ZYAN_TRUE,  ZYAN_FALSE, ZYAN_TRUE,  ZYAN_TRUE  },
        /* 32   */ { ZYAN_TRUE,  ZYAN_FALSE, ZYAN_TRUE,  ZYAN_TRUE  },
        /* 64   */ { ZYAN_TRUE,  ZYAN_FALSE, ZYAN_TRUE,  ZYAN_TRUE  },
    };
    if (!branch_lookup[request->branch_width][request->branch_type])
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (request->machine_mode == ZYDIS_MACHINE_MODE_LONG_64)
    {
        if (request->address_size_hint == ZYDIS_ADDRESS_SIZE_HINT_16)
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }
    else
    {
        if ((request->branch_width == ZYDIS_BRANCH_WIDTH_64) ||
            (request->address_size_hint == ZYDIS_ADDRESS_SIZE_HINT_64) ||
            (request->operand_size_hint == ZYDIS_OPERAND_SIZE_HINT_64))
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }

    for (ZyanU8 i = 0; i < request->operand_count; ++i)
    {
        const ZydisEncoderOperand *op = &request->operands[i];
        if ((op->type == ZYDIS_OPERAND_TYPE_UNUSED) ||
            ((ZyanUSize)op->type > ZYDIS_OPERAND_TYPE_MAX_VALUE))
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }

        switch (op->type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            if (op->reg.value > ZYDIS_REGISTER_MAX_VALUE)
            {
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            if (((ZyanUSize)op->mem.base > ZYDIS_REGISTER_MAX_VALUE) ||
                ((ZyanUSize)op->mem.index > ZYDIS_REGISTER_MAX_VALUE) ||
                !ZydisIsScaleValid(op->mem.scale))
            {
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
            break;
        case ZYDIS_OPERAND_TYPE_POINTER:
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            break;
        default:
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Encodes instruction with semantics specified in encoder request structure.
 *
 * @param   request     A pointer to the `ZydisEncoderRequest` struct. Must be validated before
 *                      calling this function.
 * @param   buffer      A pointer to the output buffer receiving encoded instruction.
 * @param   length      A pointer to the variable containing length of the output buffer. Upon
 *                      successful return this variable receives length of the encoded instruction.
 * @param   instruction Internal state of the encoder.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZydisEncoderEncodeInstructionInternal(const ZydisEncoderRequest *request,
    void *buffer, ZyanUSize *length, ZydisEncoderInstruction *instruction)
{
    ZydisEncoderInstructionMatch match;
    ZYAN_CHECK(ZydisFindMatchingDefinition(request, &match));
    ZydisEncoderBuffer output;
    output.buffer = (ZyanU8 *)buffer;
    output.size = *length;
    output.offset = 0;
    ZYAN_CHECK(ZydisBuildInstruction(&match, instruction));
    ZYAN_CHECK(ZydisEmitInstruction(instruction, &output));
    *length = output.offset;
    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

ZYDIS_EXPORT ZyanStatus ZydisEncoderEncodeInstruction(const ZydisEncoderRequest *request,
    void *buffer, ZyanUSize *length)
{
    if (!request || !buffer || !length)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    ZYAN_CHECK(ZydisEncoderCheckRequestSanity(request));

    ZydisEncoderInstruction instruction;
    return ZydisEncoderEncodeInstructionInternal(request, buffer, length, &instruction);
}

ZYDIS_EXPORT ZyanStatus ZydisEncoderEncodeInstructionAbsolute(ZydisEncoderRequest *request,
    void *buffer, ZyanUSize *length, ZyanU64 runtime_address)
{
    if (!request || !buffer || !length)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    ZYAN_CHECK(ZydisEncoderCheckRequestSanity(request));

    const ZydisEncoderRelInfo *rel_info = ZydisGetRelInfo(request->mnemonic);
    ZydisEncoderOperand *op_rip_rel = ZYAN_NULL;
    ZyanBool adjusted_rel = ZYAN_FALSE;
    ZyanU64 absolute_address = 0;
    ZyanU8 mode_index = ZydisGetMachineModeWidth(request->machine_mode) >> 5;
    for (ZyanU8 i = 0; i < request->operand_count; ++i)
    {
        ZydisEncoderOperand *op = &request->operands[i];
        if ((op->type == ZYDIS_OPERAND_TYPE_IMMEDIATE) && rel_info)
        {
            if (adjusted_rel)
            {
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }

            switch (rel_info->accepts_scaling_hints)
            {
            case ZYDIS_SIZE_HINT_NONE:
            case ZYDIS_SIZE_HINT_OSZ:
            {
                static const ZyanI8 asz_priority[3][3] =
                {
                    { 0, 1, 2 },
                    { 0, 2, 1 },
                    { 0, 2, -1 },
                };
                static const ZyanI8 osz_priority[3][3] =
                {
                    { 0, 1, 2 },
                    { 0, 2, 1 },
                    { 0, 2, 1 },
                };
                ZyanI8 forced_priority_row[3] = { -1, -1, -1 };
                ZyanI8 *priority_row = ZYAN_NULL;
                ZyanU8 extra_length = 0;
                ZyanU8 start_offset = 0;
                if (rel_info->accepts_scaling_hints == ZYDIS_SIZE_HINT_NONE)
                {
                    if ((request->branch_type == ZYDIS_BRANCH_TYPE_FAR) ||
                        (request->branch_width == ZYDIS_BRANCH_WIDTH_64))
                    {
                        return ZYAN_STATUS_INVALID_ARGUMENT;
                    }
                    if ((rel_info->accepts_branch_hints) &&
                        (request->prefixes & (ZYDIS_ATTRIB_HAS_BRANCH_NOT_TAKEN |
                                              ZYDIS_ATTRIB_HAS_BRANCH_TAKEN)))
                    {
                        extra_length = 1;
                    }
                    if (request->branch_width == ZYDIS_BRANCH_WIDTH_NONE)
                    {
                        if (request->branch_type == ZYDIS_BRANCH_TYPE_NEAR)
                        {
                            start_offset = 1;
                        }
                        priority_row = (ZyanI8 *)&asz_priority[mode_index];
                    }
                    else
                    {
                        forced_priority_row[0] = (ZyanI8)(request->branch_width - 1);
                        priority_row = (ZyanI8 *)&forced_priority_row;
                    }
                }
                else
                {
                    if (request->operand_size_hint == ZYDIS_OPERAND_SIZE_HINT_NONE)
                    {
                        priority_row = (ZyanI8 *)&osz_priority[mode_index];
                    }
                    else
                    {
                        if (request->operand_size_hint == ZYDIS_OPERAND_SIZE_HINT_64)
                        {
                            extra_length = 1;
                            forced_priority_row[0] = 2;
                        }
                        else
                        {
                            forced_priority_row[0] = (ZyanI8)(request->operand_size_hint - 1);
                        }
                        priority_row = (ZyanI8 *)&forced_priority_row;
                    }
                }
                ZYAN_ASSERT(ZYAN_ARRAY_LENGTH(asz_priority[0]) ==
                            ZYAN_ARRAY_LENGTH(osz_priority[0]));
                for (ZyanU8 j = start_offset; j < ZYAN_ARRAY_LENGTH(asz_priority[0]); ++j)
                {
                    ZyanI8 size_index = priority_row[j];
                    if (size_index < 0)
                    {
                        break;
                    }
                    ZyanU8 base_size = rel_info->size[mode_index][size_index];
                    if (base_size == 0)
                    {
                        continue;
                    }
                    ZyanU8 predicted_size = base_size + extra_length;
                    if (runtime_address > ZYAN_UINT64_MAX - predicted_size + 1)
                    {
                        continue;
                    }
                    ZyanI64 rel = (ZyanI64)(op->imm.u - (runtime_address + predicted_size));
                    ZyanU8 rel_size = ZydisGetSignedImmSize(rel);
                    if (rel_size > (8 << size_index))
                    {
                        continue;
                    }
                    op->imm.s = rel;
                    adjusted_rel = ZYAN_TRUE;
                    break;
                }
                break;
            }
            case ZYDIS_SIZE_HINT_ASZ:
            {
                static const ZyanI8 asz_prefix_lookup[3][ZYDIS_ADDRESS_SIZE_HINT_MAX_VALUE + 1] =
                {
                    { 0, 0, 1, -1 },
                    { 0, 1, 0, -1 },
                    { 0, -1, 1, 0 },
                };
                ZyanI8 extra_length = asz_prefix_lookup[mode_index][request->address_size_hint];
                if (extra_length < 0)
                {
                    return ZYAN_STATUS_INVALID_ARGUMENT;
                }
                ZyanU8 asz_index = (request->address_size_hint == ZYDIS_ADDRESS_SIZE_HINT_NONE)
                    ? mode_index
                    : ZydisGetAszFromHint(request->address_size_hint) >> 5;
                ZYAN_ASSERT((rel_info->size[asz_index][0] != 0) &&
                            (rel_info->size[asz_index][1] == 0) &&
                            (rel_info->size[asz_index][2] == 0) &&
                            !rel_info->accepts_branch_hints);
                ZyanU8 predicted_size = rel_info->size[asz_index][0] + extra_length;
                if (runtime_address > ZYAN_UINT64_MAX - predicted_size + 1)
                {
                    return ZYAN_STATUS_INVALID_ARGUMENT;
                }
                ZyanI64 rel = (ZyanI64)(op->imm.u - (runtime_address + predicted_size));
                ZyanU8 rel_size = ZydisGetSignedImmSize(rel);
                if (rel_size > 8)
                {
                    return ZYAN_STATUS_INVALID_ARGUMENT;
                }
                op->imm.s = rel;
                adjusted_rel = ZYAN_TRUE;
                break;
            }
            default:
                ZYAN_UNREACHABLE;
            }
            if (!adjusted_rel)
            {
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
        }
        else if ((op->type == ZYDIS_OPERAND_TYPE_MEMORY) &&
                 ((op->mem.base == ZYDIS_REGISTER_EIP) ||
                  (op->mem.base == ZYDIS_REGISTER_RIP)))
        {
            if (op_rip_rel)
            {
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }

            absolute_address = op->mem.displacement;
            op->mem.displacement = 0;
            op_rip_rel = op;
        }
    }

    ZydisEncoderInstruction instruction;
    ZYAN_CHECK(ZydisEncoderEncodeInstructionInternal(request, buffer, length, &instruction));
    if (op_rip_rel)
    {
        ZyanUSize instruction_size = *length;
        if (runtime_address > ZYAN_UINT64_MAX - instruction_size + 1)
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
        ZyanI64 rip_rel = (ZyanI64)(absolute_address - (runtime_address + instruction_size));
        if (ZydisGetSignedImmSize(rip_rel) > 32)
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
        ZYAN_ASSERT(instruction.disp_size != 0);
        ZyanU8 disp_offset = (instruction.disp_size >> 3) + (instruction.imm_size >> 3);
        ZYAN_ASSERT(instruction_size > disp_offset);
        ZYAN_MEMCPY((ZyanU8 *)buffer + instruction_size - disp_offset, &rip_rel, sizeof(ZyanI32));
        op_rip_rel->mem.displacement = rip_rel;
    }

    return ZYAN_STATUS_SUCCESS;
}

ZYDIS_EXPORT ZyanStatus ZydisEncoderDecodedInstructionToEncoderRequest(
    const ZydisDecodedInstruction *instruction, const ZydisDecodedOperand* operands, 
    ZyanU8 operand_count, ZydisEncoderRequest *request)
{
    if (!instruction || !request || (operand_count && !operands))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_MEMSET(request, 0, sizeof(ZydisEncoderRequest));
    request->machine_mode = instruction->machine_mode;
    request->mnemonic = instruction->mnemonic;
    request->prefixes = instruction->attributes & ZYDIS_ENCODABLE_PREFIXES;
    request->branch_type = instruction->meta.branch_type;
    if (!(instruction->attributes & ZYDIS_ATTRIB_ACCEPTS_SEGMENT))
    {
        request->prefixes &= ~ZYDIS_ATTRIB_HAS_SEGMENT;
    }

    switch (instruction->address_width)
    {
    case 16:
        request->address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_16;
        break;
    case 32:
        request->address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_32;
        break;
    case 64:
        request->address_size_hint = ZYDIS_ADDRESS_SIZE_HINT_64;
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    switch (instruction->operand_width)
    {
    case 8:
        request->operand_size_hint = ZYDIS_OPERAND_SIZE_HINT_8;
        break;
    case 16:
        request->operand_size_hint = ZYDIS_OPERAND_SIZE_HINT_16;
        break;
    case 32:
        request->operand_size_hint = ZYDIS_OPERAND_SIZE_HINT_32;
        break;
    case 64:
        request->operand_size_hint = ZYDIS_OPERAND_SIZE_HINT_64;
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    switch (request->branch_type)
    {
    case ZYDIS_BRANCH_TYPE_NONE:
        request->branch_width = ZYDIS_BRANCH_WIDTH_NONE;
        break;
    case ZYDIS_BRANCH_TYPE_SHORT:
        request->branch_width = ZYDIS_BRANCH_WIDTH_8;
        break;
    case ZYDIS_BRANCH_TYPE_NEAR:
    case ZYDIS_BRANCH_TYPE_FAR:
        switch (instruction->operand_width)
        {
        case 16:
            request->branch_width = ZYDIS_BRANCH_WIDTH_16;
            break;
        case 32:
            request->branch_width = ZYDIS_BRANCH_WIDTH_32;
            break;
        case 64:
            request->branch_width = ZYDIS_BRANCH_WIDTH_64;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        request->evex.broadcast = !instruction->avx.broadcast.is_static ?
            instruction->avx.broadcast.mode : ZYDIS_BROADCAST_MODE_INVALID;
        request->evex.rounding = instruction->avx.rounding.mode;
        request->evex.sae = instruction->avx.has_sae;
        request->evex.zeroing_mask = (instruction->avx.mask.mode == ZYDIS_MASK_MODE_ZEROING ||
            instruction->avx.mask.mode == ZYDIS_MASK_MODE_CONTROL_ZEROING) &&
            (instruction->raw.evex.z) ? ZYAN_TRUE : ZYAN_FALSE;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        request->mvex.broadcast = !instruction->avx.broadcast.is_static ?
            instruction->avx.broadcast.mode : ZYDIS_BROADCAST_MODE_INVALID;
        request->mvex.conversion = instruction->avx.conversion.mode;
        request->mvex.rounding = instruction->avx.rounding.mode;
        request->mvex.swizzle = instruction->avx.swizzle.mode;
        request->mvex.sae = instruction->avx.has_sae;
        request->mvex.eviction_hint = instruction->avx.has_eviction_hint;
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    request->allowed_encodings = 1 << instruction->encoding;

    if ((operand_count > ZYDIS_ENCODER_MAX_OPERANDS) || 
        (operand_count > instruction->operand_count_visible))
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    request->operand_count = operand_count;
    for (ZyanU8 i = 0; i < operand_count; ++i)
    {
        const ZydisDecodedOperand *dec_op = &operands[i];
        ZydisEncoderOperand *enc_op = &request->operands[i];

        enc_op->type = dec_op->type;
        switch (dec_op->type)
        {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            enc_op->reg.value = dec_op->reg.value;
            enc_op->reg.is4 = dec_op->encoding == ZYDIS_OPERAND_ENCODING_IS4;
            break;
        case ZYDIS_OPERAND_TYPE_MEMORY:
            enc_op->mem.base = dec_op->mem.base;
            enc_op->mem.index = dec_op->mem.index;
            enc_op->mem.scale = dec_op->mem.type != ZYDIS_MEMOP_TYPE_MIB ? dec_op->mem.scale : 0;
            if (dec_op->encoding == ZYDIS_OPERAND_ENCODING_DISP16_32_64)
            {
                ZydisCalcAbsoluteAddress(instruction, dec_op, 0,
                    (ZyanU64 *)&enc_op->mem.displacement);
            }
            else
            {
                enc_op->mem.displacement = dec_op->mem.disp.has_displacement ?
                    dec_op->mem.disp.value : 0;
            }
            enc_op->mem.size = dec_op->size / 8;
            break;
        case ZYDIS_OPERAND_TYPE_POINTER:
            enc_op->ptr.segment = dec_op->ptr.segment;
            enc_op->ptr.offset = dec_op->ptr.offset;
            break;
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            enc_op->imm.u = dec_op->imm.value.u;
            // `XBEGIN` is an ISA-wide unique instruction because it's not a branching instruction
            // but it has a relative operand which behaves differently from all other relatives
            // (no truncating behavior in 16-bit mode). Encoder treats it as non-branching
            // instruction that scales with hidden operand size.
            if ((dec_op->imm.is_relative) &&
                (instruction->mnemonic != ZYDIS_MNEMONIC_XBEGIN))
            {
                switch (instruction->raw.imm->size)
                {
                case 8:
                    request->branch_width = ZYDIS_BRANCH_WIDTH_8;
                    break;
                case 16:
                    request->branch_width = ZYDIS_BRANCH_WIDTH_16;
                    break;
                case 32:
                    request->branch_width = ZYDIS_BRANCH_WIDTH_32;
                    break;
                default:
                    return ZYAN_STATUS_INVALID_ARGUMENT;
                }
            }
            break;
        default:
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }

    return ZYAN_STATUS_SUCCESS;
}

ZYDIS_EXPORT ZyanStatus ZydisEncoderNopFill(void *buffer, ZyanUSize length)
{
    if (!buffer)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    // Intel SDM Vol. 2B "Recommended Multi-Byte Sequence of NOP Instruction"
    static const ZyanU8 nops[9][9] =
    {
        { 0x90 },
        { 0x66, 0x90 },
        { 0x0F, 0x1F, 0x00 },
        { 0x0F, 0x1F, 0x40, 0x00 },
        { 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00 },
        { 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
        { 0x66, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00 },
    };

    ZyanU8 *output = (ZyanU8 *)buffer;
    while (length)
    {
        ZyanUSize nop_size = (length > 9) ? 9 : length;
        ZYAN_MEMCPY(output, nops[nop_size - 1], nop_size);
        output += nop_size;
        length -= nop_size;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
