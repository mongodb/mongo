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

// Signed integer overflow is expected behavior in this function, for wrapping around the
// instruction pointer on jumps right at the end of the address space.
ZYAN_NO_SANITIZE("signed-integer-overflow")
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
                // `XBEGIN` is a special case as it doesn't truncate computed address
                // This behavior is documented by Intel (SDM Vol. 2C):
                // Use of the 16-bit operand size does not cause this address to be truncated to
                // 16 bits, unlike a near jump to a relative offset.
                if ((instruction->operand_width == 16) &&
                    (instruction->mnemonic != ZYDIS_MNEMONIC_XBEGIN))
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

/* ============================================================================================== */
