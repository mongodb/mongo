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

#include "zydis/Zycore/LibC.h"
#include "zydis/Zydis/Utils.h"

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Address calculation                                                                            */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisCalcAbsoluteAddress(const ZydisDecodedInstruction* instruction,
    const ZydisDecodedOperand* operand, ZyanU64 runtime_address, ZyanU64* result_address)
{
    if (!instruction || !operand || !result_address)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    switch (operand->type)
    {
    case ZYDIS_OPERAND_TYPE_MEMORY:
        if (!operand->mem.disp.has_displacement)
        {
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
        if (operand->mem.base == ZYDIS_REGISTER_EIP)
        {
            *result_address = ((ZyanU32)runtime_address + instruction->length +
                (ZyanU32)operand->mem.disp.value);
            return ZYAN_STATUS_SUCCESS;
        }
        if (operand->mem.base == ZYDIS_REGISTER_RIP)
        {
            *result_address = (ZyanU64)(runtime_address + instruction->length +
                operand->mem.disp.value);
            return ZYAN_STATUS_SUCCESS;
        }
        if ((operand->mem.base == ZYDIS_REGISTER_NONE) &&
            (operand->mem.index == ZYDIS_REGISTER_NONE))
        {
            switch (instruction->address_width)
            {
            case 16:
                *result_address = (ZyanU64)operand->mem.disp.value & 0x000000000000FFFF;
                return ZYAN_STATUS_SUCCESS;
            case 32:
                *result_address = (ZyanU64)operand->mem.disp.value & 0x00000000FFFFFFFF;
                return ZYAN_STATUS_SUCCESS;
            case 64:
                *result_address = (ZyanU64)operand->mem.disp.value;
                return ZYAN_STATUS_SUCCESS;
            default:
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
        }
        break;
    case ZYDIS_OPERAND_TYPE_IMMEDIATE:
        if (operand->imm.is_signed && operand->imm.is_relative)
        {
            *result_address = (ZyanU64)((ZyanI64)runtime_address + instruction->length +
                operand->imm.value.s);
            switch (instruction->machine_mode)
            {
            case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
            case ZYDIS_MACHINE_MODE_LEGACY_16:
            case ZYDIS_MACHINE_MODE_REAL_16:
            case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
            case ZYDIS_MACHINE_MODE_LEGACY_32:
                if (operand->size == 16)
                {
                    *result_address &= 0xFFFF;
                }
                break;
            case ZYDIS_MACHINE_MODE_LONG_64:
                break;
            default:
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
            return ZYAN_STATUS_SUCCESS;
        }
        break;
    default:
        break;
    }

    return ZYAN_STATUS_INVALID_ARGUMENT;
}

ZyanStatus ZydisCalcAbsoluteAddressEx(const ZydisDecodedInstruction* instruction,
    const ZydisDecodedOperand* operand, ZyanU64 runtime_address,
    const ZydisRegisterContext* register_context, ZyanU64* result_address)
{
    // TODO: Test this with AGEN/MIB operands
    // TODO: Add support for Gather/Scatter instructions

    if (!instruction || !operand || !register_context || !result_address)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if ((operand->type != ZYDIS_OPERAND_TYPE_MEMORY) ||
        ((operand->mem.base == ZYDIS_REGISTER_NONE) &&
         (operand->mem.index == ZYDIS_REGISTER_NONE)) ||
        (operand->mem.base == ZYDIS_REGISTER_EIP) ||
        (operand->mem.base == ZYDIS_REGISTER_RIP))
    {
        return ZydisCalcAbsoluteAddress(instruction, operand, runtime_address, result_address);
    }

    ZyanU64 value = operand->mem.disp.value;
    if (operand->mem.base)
    {
        value += register_context->values[operand->mem.base];
    }
    if (operand->mem.index)
    {
        value += register_context->values[operand->mem.index] * operand->mem.scale;
    }

    switch (instruction->address_width)
    {
    case 16:
        *result_address = value & 0x000000000000FFFF;
        return ZYAN_STATUS_SUCCESS;
    case 32:
        *result_address = value & 0x00000000FFFFFFFF;
        return ZYAN_STATUS_SUCCESS;
    case 64:
        *result_address = value;
        return ZYAN_STATUS_SUCCESS;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Accessed CPU flags                                                                             */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisGetAccessedFlagsByAction(const ZydisDecodedInstruction* instruction,
    ZydisCPUFlagAction action, ZydisCPUFlags* flags)
{
    if (!instruction || !flags)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *flags = 0;
    for (ZyanUSize i = 0; i < ZYAN_ARRAY_LENGTH(instruction->accessed_flags); ++i)
    {
        if (instruction->accessed_flags[i].action == action)
        {
            *flags |= (1 << i);
        }
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisGetAccessedFlagsRead(const ZydisDecodedInstruction* instruction,
    ZydisCPUFlags* flags)
{
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_TESTED, flags));
    ZydisCPUFlags temp;
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_TESTED_MODIFIED,
        &temp));
    *flags |= temp;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisGetAccessedFlagsWritten(const ZydisDecodedInstruction* instruction,
    ZydisCPUFlags* flags)
{
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_TESTED_MODIFIED,
        flags));
    ZydisCPUFlags temp;
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_MODIFIED, &temp));
    *flags |= temp;
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_SET_0, &temp));
    *flags |= temp;
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_SET_1, &temp));
    *flags |= temp;
    ZYAN_CHECK(ZydisGetAccessedFlagsByAction(instruction, ZYDIS_CPUFLAG_ACTION_UNDEFINED, &temp));
    *flags |= temp;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Instruction segments                                                                           */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisGetInstructionSegments(const ZydisDecodedInstruction* instruction,
    ZydisInstructionSegments* segments)
{
    if (!instruction || !segments)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_MEMSET(segments, 0, sizeof(*segments));

    // Legacy prefixes and `REX`
    if (instruction->raw.prefix_count)
    {
        const ZyanU8 rex_offset = (instruction->attributes & ZYDIS_ATTRIB_HAS_REX) ? 1 : 0;
        if (!rex_offset || (instruction->raw.prefix_count > 1))
        {
            segments->segments[segments->count  ].type   = ZYDIS_INSTR_SEGMENT_PREFIXES;
            segments->segments[segments->count  ].offset = 0;
            segments->segments[segments->count++].size   =
                instruction->raw.prefix_count - rex_offset;
        }
        if (rex_offset)
        {
            segments->segments[segments->count  ].type   = ZYDIS_INSTR_SEGMENT_REX;
            segments->segments[segments->count  ].offset =
                instruction->raw.prefix_count - rex_offset;
            segments->segments[segments->count++].size   = 1;
        }
    }

    // Encoding prefixes
    ZydisInstructionSegment segment_type = ZYDIS_INSTR_SEGMENT_NONE;
    ZyanU8 segment_offset = 0;
    ZyanU8 segment_size = 0;
    switch (instruction->encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        segment_type = ZYDIS_INSTR_SEGMENT_XOP;
        segment_offset = instruction->raw.xop.offset;
        segment_size = 3;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        segment_type = ZYDIS_INSTR_SEGMENT_VEX;
        segment_offset = instruction->raw.vex.offset;
        segment_size = instruction->raw.vex.size;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        segment_type = ZYDIS_INSTR_SEGMENT_EVEX;
        segment_offset = instruction->raw.evex.offset;
        segment_size = 4;
        break;
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        segment_type = ZYDIS_INSTR_SEGMENT_MVEX;
        segment_offset = instruction->raw.mvex.offset;
        segment_size = 4;
        break;
    default:
        break;
    }
    if (segment_type)
    {
        segments->segments[segments->count  ].type   = segment_type;
        segments->segments[segments->count  ].offset = segment_offset;
        segments->segments[segments->count++].size   = segment_size;
    }

    // Opcode
    segment_size = 1;
    if ((instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_LEGACY) ||
        (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_3DNOW))
    {
        switch (instruction->opcode_map)
        {
        case ZYDIS_OPCODE_MAP_DEFAULT:
            break;
        case ZYDIS_OPCODE_MAP_0F:
            ZYAN_FALLTHROUGH;
        case ZYDIS_OPCODE_MAP_0F0F:
            segment_size = 2;
            break;
        case ZYDIS_OPCODE_MAP_0F38:
            ZYAN_FALLTHROUGH;
        case ZYDIS_OPCODE_MAP_0F3A:
            segment_size = 3;
            break;
        default:
            ZYAN_UNREACHABLE;
        }
    }
    segments->segments[segments->count  ].type = ZYDIS_INSTR_SEGMENT_OPCODE;
    if (segments->count)
    {
        segments->segments[segments->count].offset =
            segments->segments[segments->count - 1].offset +
            segments->segments[segments->count - 1].size;
    } else
    {
        segments->segments[segments->count].offset = 0;
    }
    segments->segments[segments->count++].size = segment_size;

    // ModRM
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_MODRM)
    {
        segments->segments[segments->count  ].type = ZYDIS_INSTR_SEGMENT_MODRM;
        segments->segments[segments->count  ].offset = instruction->raw.modrm.offset;
        segments->segments[segments->count++].size = 1;
    }

    // SIB
    if (instruction->attributes & ZYDIS_ATTRIB_HAS_SIB)
    {
        segments->segments[segments->count  ].type = ZYDIS_INSTR_SEGMENT_SIB;
        segments->segments[segments->count  ].offset = instruction->raw.sib.offset;
        segments->segments[segments->count++].size = 1;
    }

    // Displacement
    if (instruction->raw.disp.size)
    {
        segments->segments[segments->count  ].type = ZYDIS_INSTR_SEGMENT_DISPLACEMENT;
        segments->segments[segments->count  ].offset = instruction->raw.disp.offset;
        segments->segments[segments->count++].size = instruction->raw.disp.size / 8;
    }

    // Immediates
    for (ZyanU8 i = 0; i < 2; ++i)
    {
        if (instruction->raw.imm[i].size)
        {
            segments->segments[segments->count  ].type = ZYDIS_INSTR_SEGMENT_IMMEDIATE;
            segments->segments[segments->count  ].offset = instruction->raw.imm[i].offset;
            segments->segments[segments->count++].size = instruction->raw.imm[i].size / 8;
        }
    }

    if (instruction->encoding == ZYDIS_INSTRUCTION_ENCODING_3DNOW)
    {
        segments->segments[segments->count].type = ZYDIS_INSTR_SEGMENT_OPCODE;
        segments->segments[segments->count].offset = instruction->length -1;
        segments->segments[segments->count++].size = 1;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
