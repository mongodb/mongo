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
#include "zydis/Zydis/Segment.h"

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

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
