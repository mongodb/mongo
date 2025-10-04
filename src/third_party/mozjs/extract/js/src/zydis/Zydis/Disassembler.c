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

#include "zydis/Zydis/Disassembler.h"
#include "zydis/Zycore/LibC.h"

/* ============================================================================================== */
/* Internal helpers                                                                               */
/* ============================================================================================== */

static ZyanStatus ZydisDisassemble(ZydisMachineMode machine_mode,
    ZyanU64 runtime_address, const void* buffer, ZyanUSize length,
    ZydisDisassembledInstruction *instruction, ZydisFormatterStyle style)
{
    if (!buffer || !instruction)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *instruction = (ZydisDisassembledInstruction)
    {
      .runtime_address = runtime_address
    };

    // Derive the stack width from the address width.
    ZydisStackWidth stack_width;
    switch (machine_mode)
    {
    case ZYDIS_MACHINE_MODE_LONG_64:
        stack_width = ZYDIS_STACK_WIDTH_64;
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_32:
    case ZYDIS_MACHINE_MODE_LEGACY_32:
        stack_width = ZYDIS_STACK_WIDTH_32;
        break;
    case ZYDIS_MACHINE_MODE_LONG_COMPAT_16:
    case ZYDIS_MACHINE_MODE_LEGACY_16:
    case ZYDIS_MACHINE_MODE_REAL_16:
        stack_width = ZYDIS_STACK_WIDTH_16;
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZydisDecoder decoder;
    ZYAN_CHECK(ZydisDecoderInit(&decoder, machine_mode, stack_width));

    ZydisDecoderContext ctx;
    ZYAN_CHECK(ZydisDecoderDecodeInstruction(&decoder, &ctx, buffer, length, &instruction->info));
    ZYAN_CHECK(ZydisDecoderDecodeOperands(&decoder, &ctx, &instruction->info,
        instruction->operands, instruction->info.operand_count));

    ZydisFormatter formatter;
    ZYAN_CHECK(ZydisFormatterInit(&formatter, style));
    ZYAN_CHECK(ZydisFormatterFormatInstruction(&formatter, &instruction->info,
        instruction->operands, instruction->info.operand_count_visible, instruction->text,
        sizeof(instruction->text), runtime_address, ZYAN_NULL));

    return ZYAN_STATUS_SUCCESS;
}

/* ============================================================================================== */
/* Public functions                                                                               */
/* ============================================================================================== */

ZyanStatus ZydisDisassembleIntel(ZydisMachineMode machine_mode,
    ZyanU64 runtime_address, const void* buffer, ZyanUSize length,
    ZydisDisassembledInstruction *instruction)
{
    return ZydisDisassemble(machine_mode, runtime_address, buffer, length, instruction,
        ZYDIS_FORMATTER_STYLE_INTEL);
}

ZyanStatus ZydisDisassembleATT(ZydisMachineMode machine_mode,
    ZyanU64 runtime_address, const void* buffer, ZyanUSize length,
    ZydisDisassembledInstruction *instruction)
{
    return ZydisDisassemble(machine_mode, runtime_address, buffer, length, instruction,
        ZYDIS_FORMATTER_STYLE_ATT);
}

/* ============================================================================================== */
