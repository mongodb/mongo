/***************************************************************************************************

  Zyan Disassembler Library (Zydis)

  Original Author : Florian Bernd, Joel Hoener

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

#include "zydis/Zydis/Internal/FormatterBase.h"
#include "zydis/Zydis/Utils.h"

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

#include "zydis/Zydis/Generated/FormatterStrings.inc"

static const ZydisShortString* const STR_PREF_REX[16] =
{
    &STR_PREF_REX_40,
    &STR_PREF_REX_41,
    &STR_PREF_REX_42,
    &STR_PREF_REX_43,
    &STR_PREF_REX_44,
    &STR_PREF_REX_45,
    &STR_PREF_REX_46,
    &STR_PREF_REX_47,
    &STR_PREF_REX_48,
    &STR_PREF_REX_49,
    &STR_PREF_REX_4A,
    &STR_PREF_REX_4B,
    &STR_PREF_REX_4C,
    &STR_PREF_REX_4D,
    &STR_PREF_REX_4E,
    &STR_PREF_REX_4F
};

static const ZydisPredefinedToken* const TOK_PREF_REX[16] =
{
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_40,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_41,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_42,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_43,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_44,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_45,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_46,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_47,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_48,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_49,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4A,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4B,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4C,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4D,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4E,
    (const ZydisPredefinedToken* const)&TOK_DATA_PREF_REX_4F
};

/* ============================================================================================== */
/* Helper functions                                                                               */
/* ============================================================================================== */

ZyanU32 ZydisFormatterHelperGetExplicitSize(const ZydisFormatter* formatter,
    ZydisFormatterContext* context, const ZydisDecodedOperand* operand)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(context);
    ZYAN_ASSERT(operand);

    ZYAN_ASSERT(operand->type == ZYDIS_OPERAND_TYPE_MEMORY);
    ZYAN_ASSERT((operand->mem.type == ZYDIS_MEMOP_TYPE_MEM) ||
                (operand->mem.type == ZYDIS_MEMOP_TYPE_VSIB));

    if (formatter->force_memory_size)
    {
        return operand->size;
    }

    if (!context->operands)
    {
        // Single operand formatting. We can not derive the explicit size by using the other
        // operands.
        return 0;
    }

    switch (operand->id)
    {
    case 0:
        if (context->instruction->operand_count_visible < 2)
        {
            return 0;
        }
        if ((context->operands[1].type == ZYDIS_OPERAND_TYPE_UNUSED) ||
            (context->operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE))
        {
            return context->operands[0].size;
        }
        if (context->operands[0].size != context->operands[1].size)
        {
            return context->operands[0].size;
        }
        if ((context->operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) &&
            (context->operands[1].visibility == ZYDIS_OPERAND_VISIBILITY_IMPLICIT) &&
            (context->operands[1].reg.value == ZYDIS_REGISTER_CL))
        {
            return context->operands[0].size;
        }
        break;
    case 1:
    case 2:
        if (context->operands[operand->id - 1].size !=
            context->operands[operand->id].size)
        {
            return context->operands[operand->id].size;
        }
        break;
    default:
        break;
    }

    return 0;
}

/* ============================================================================================== */
/* Formatter functions                                                                            */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Operands                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisFormatterBaseFormatOperandREG(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    return formatter->func_print_register(formatter, buffer, context, context->operand->reg.value);
}

ZyanStatus ZydisFormatterBaseFormatOperandPTR(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_IMMEDIATE);
    ZYDIS_STRING_APPEND_NUM_U(formatter, formatter->addr_base, &buffer->string,
        context->operand->ptr.segment, 4, formatter->hex_force_leading_number);
    ZYDIS_BUFFER_APPEND(buffer, DELIM_SEGMENT);

    ZyanU8 padding;
    switch (context->instruction->operand_width)
    {
    case 16:
        padding = 4;
        break;
    case 32:
        padding = 8;
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_IMMEDIATE);
    ZYDIS_STRING_APPEND_NUM_U(formatter, formatter->addr_base, &buffer->string,
        context->operand->ptr.offset , padding, formatter->hex_force_leading_number);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisFormatterBaseFormatOperandIMM(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    // The immediate operand contains an address
    if (context->operand->imm.is_relative)
    {
        const ZyanBool absolute = !formatter->force_relative_branches &&
            (context->runtime_address != ZYDIS_RUNTIME_ADDRESS_NONE);
        if (absolute)
        {
            return formatter->func_print_address_abs(formatter, buffer, context);
        }
        return formatter->func_print_address_rel(formatter, buffer, context);
    }

    // The immediate operand contains an actual ordinal value
    return formatter->func_print_imm(formatter, buffer, context);
}

/* ---------------------------------------------------------------------------------------------- */
/* Elemental tokens                                                                               */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisFormatterBasePrintAddressABS(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    ZyanU64 address;
    ZYAN_CHECK(ZydisCalcAbsoluteAddress(context->instruction, context->operand,
        context->runtime_address, &address));
    ZyanU8 padding = (formatter->addr_padding_absolute ==
        ZYDIS_PADDING_AUTO) ? 0 : (ZyanU8)formatter->addr_padding_absolute;
    if ((formatter->addr_padding_absolute == ZYDIS_PADDING_AUTO) &&
        (formatter->addr_base == ZYDIS_NUMERIC_BASE_HEX))
    {
        switch (context->instruction->stack_width)
        {
        case 16:
            padding =  4;
            address = (ZyanU16)address;
            break;
        case 32:
            padding =  8;
            address = (ZyanU32)address;
            break;
        case 64:
            padding = 16;
            break;
        default:
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }

    ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_ADDRESS_ABS);
    ZYDIS_STRING_APPEND_NUM_U(formatter, formatter->addr_base, &buffer->string, address, padding,
        formatter->hex_force_leading_number);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisFormatterBasePrintAddressREL(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    ZyanU64 address;
    ZYAN_CHECK(ZydisCalcAbsoluteAddress(context->instruction, context->operand, 0, &address));

    ZyanU8 padding = (formatter->addr_padding_relative ==
        ZYDIS_PADDING_AUTO) ? 0 : (ZyanU8)formatter->addr_padding_relative;
    if ((formatter->addr_padding_relative == ZYDIS_PADDING_AUTO) &&
        (formatter->addr_base == ZYDIS_NUMERIC_BASE_HEX))
    {
        switch (context->instruction->stack_width)
        {
        case 16:
            padding =  4;
            address = (ZyanU16)address;
            break;
        case 32:
            padding =  8;
            address = (ZyanU32)address;
            break;
        case 64:
            padding = 16;
            break;
        default:
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
    }

    ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_ADDRESS_REL);
    switch (formatter->addr_signedness)
    {
    case ZYDIS_SIGNEDNESS_AUTO:
    case ZYDIS_SIGNEDNESS_SIGNED:
        ZYDIS_STRING_APPEND_NUM_S(formatter, formatter->addr_base, &buffer->string, address,
            padding, formatter->hex_force_leading_number, ZYAN_TRUE);
        break;
    case ZYDIS_SIGNEDNESS_UNSIGNED:
        ZYAN_CHECK(ZydisStringAppendShort(&buffer->string, &STR_ADD));
        ZYDIS_STRING_APPEND_NUM_U(formatter, formatter->addr_base, &buffer->string, address,
            padding, formatter->hex_force_leading_number);
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisFormatterBasePrintIMM(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_IMMEDIATE);

    const ZyanBool is_signed =
        (formatter->imm_signedness == ZYDIS_SIGNEDNESS_SIGNED) ||
        (formatter->imm_signedness == ZYDIS_SIGNEDNESS_AUTO && (context->operand->imm.is_signed));
    if (is_signed && (context->operand->imm.value.s < 0))
    {
        ZYDIS_STRING_APPEND_NUM_S(formatter, formatter->imm_base, &buffer->string,
            context->operand->imm.value.s, formatter->imm_padding,
            formatter->hex_force_leading_number, ZYAN_FALSE);
        return ZYAN_STATUS_SUCCESS;
    }
    ZyanU64 value;
    ZyanU8 padding = (formatter->imm_padding ==
        ZYDIS_PADDING_AUTO) ? 0 : (ZyanU8)formatter->imm_padding;
    switch (context->instruction->operand_width)
    {
    case 8:
        if (formatter->imm_padding == ZYDIS_PADDING_AUTO)
        {
            padding =  2;
        }
        value = (ZyanU8 )context->operand->imm.value.u;
        break;
    case 16:
        if (formatter->imm_padding == ZYDIS_PADDING_AUTO)
        {
            padding =  4;
        }
        value = (ZyanU16)context->operand->imm.value.u;
        break;
    case 32:
        if (formatter->imm_padding == ZYDIS_PADDING_AUTO)
        {
            padding =  8;
        }
        value = (ZyanU32)context->operand->imm.value.u;
        break;
    case 64:
        if (formatter->imm_padding == ZYDIS_PADDING_AUTO)
        {
            padding = 16;
        }
        value = (ZyanU64)context->operand->imm.value.u;
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    ZYDIS_STRING_APPEND_NUM_U(formatter, formatter->imm_base, &buffer->string, value, padding,
        formatter->hex_force_leading_number);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Optional tokens                                                                                */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZydisFormatterBasePrintSegment(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    ZyanBool printed_segment = ZYAN_FALSE;
    switch (context->operand->mem.segment)
    {
    case ZYDIS_REGISTER_ES:
    case ZYDIS_REGISTER_CS:
    case ZYDIS_REGISTER_FS:
    case ZYDIS_REGISTER_GS:
        ZYAN_CHECK(formatter->func_print_register(formatter, buffer, context,
            context->operand->mem.segment));
        printed_segment = ZYAN_TRUE;
        break;
    case ZYDIS_REGISTER_SS:
        if ((formatter->force_memory_segment) ||
            (context->instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_SS))
        {
            ZYAN_CHECK(formatter->func_print_register(formatter, buffer, context,
                context->operand->mem.segment));
            printed_segment = ZYAN_TRUE;
        }
        break;
    case ZYDIS_REGISTER_DS:
        if ((formatter->force_memory_segment) ||
            (context->instruction->attributes & ZYDIS_ATTRIB_HAS_SEGMENT_DS))
        {
            ZYAN_CHECK(formatter->func_print_register(formatter, buffer, context,
                context->operand->mem.segment));
            printed_segment = ZYAN_TRUE;
        }
        break;
    default:
        break;
    }
    if (printed_segment)
    {
        ZYDIS_BUFFER_APPEND(buffer, DELIM_SEGMENT);
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisFormatterBasePrintPrefixes(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

    if (formatter->detailed_prefixes)
    {
        for (ZyanU8 i = 0; i < context->instruction->raw.prefix_count; ++i)
        {
            const ZyanU8 value = context->instruction->raw.prefixes[i].value;
            switch (context->instruction->raw.prefixes[i].type)
            {
            case ZYDIS_PREFIX_TYPE_IGNORED:
            case ZYDIS_PREFIX_TYPE_MANDATORY:
            {
                if ((value & 0xF0) == 0x40)
                {
                    if (buffer->is_token_list)
                    {
                        // TODO: Case
                        ZYAN_CHECK(ZydisFormatterBufferAppendPredefined(buffer,
                            TOK_PREF_REX[value & 0x0F]));
                    } else
                    {
                        ZYAN_CHECK(ZydisStringAppendShortCase(&buffer->string,
                            STR_PREF_REX[value & 0x0F], formatter->case_prefixes));
                    }
                } else
                {
                    switch (value)
                    {
                    case 0xF0:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_LOCK, formatter->case_prefixes);
                        break;
                    case 0x2E:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_SEG_CS, formatter->case_prefixes);
                        break;
                    case 0x36:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_SEG_SS, formatter->case_prefixes);
                        break;
                    case 0x3E:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_SEG_DS, formatter->case_prefixes);
                        break;
                    case 0x26:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_SEG_ES, formatter->case_prefixes);
                        break;
                    case 0x64:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_SEG_FS, formatter->case_prefixes);
                        break;
                    case 0x65:
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_SEG_GS, formatter->case_prefixes);
                        break;
                    default:
                        ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_PREFIX);
                        ZYAN_CHECK(ZydisStringAppendHexU(&buffer->string, value, 0,
                            formatter->hex_force_leading_number, formatter->hex_uppercase,
                            ZYAN_NULL, ZYAN_NULL));
                        ZYDIS_BUFFER_APPEND_TOKEN(buffer, ZYDIS_TOKEN_WHITESPACE);
                        ZYAN_CHECK(ZydisStringAppendShort(&buffer->string, &STR_WHITESPACE));
                        break;
                    }
                }
                break;
            }
            case ZYDIS_PREFIX_TYPE_EFFECTIVE:
                switch (value)
                {
                case 0xF0:
                    ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_LOCK, formatter->case_prefixes);
                    break;
                case 0xF2:
                    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_XACQUIRE)
                    {
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_XACQUIRE, formatter->case_prefixes);
                    }
                    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_REPNE)
                    {
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_REPNE, formatter->case_prefixes);
                    }

                    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_BND)
                    {
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_BND, formatter->case_prefixes);
                    }
                    break;
                case 0xF3:
                    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_XRELEASE)
                    {
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_XRELEASE, formatter->case_prefixes);
                    }
                    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_REP)
                    {
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_REP, formatter->case_prefixes);
                    }
                    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_REPE)
                    {
                        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_REPE, formatter->case_prefixes);
                    }
                    break;
                default:
                    break;
                }
                break;
            default:
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
        }
        return ZYAN_STATUS_SUCCESS;
    }

    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_XACQUIRE)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_XACQUIRE, formatter->case_prefixes);
    }
    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_XRELEASE)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_XRELEASE, formatter->case_prefixes);
    }

    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_LOCK)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_LOCK, formatter->case_prefixes);
    }

    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_BND)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_BND, formatter->case_prefixes);
    }

    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_NOTRACK)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_NOTRACK, formatter->case_prefixes);
    }

    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_REP)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_REP, formatter->case_prefixes);
        return ZYAN_STATUS_SUCCESS;
    }
    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_REPE)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_REPE, formatter->case_prefixes);
        return ZYAN_STATUS_SUCCESS;
    }
    if (context->instruction->attributes & ZYDIS_ATTRIB_HAS_REPNE)
    {
        ZYDIS_BUFFER_APPEND_CASE(buffer, PREF_REPNE, formatter->case_prefixes);
        return ZYAN_STATUS_SUCCESS;
    }

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZydisFormatterBasePrintDecorator(const ZydisFormatter* formatter,
    ZydisFormatterBuffer* buffer, ZydisFormatterContext* context, ZydisDecorator decorator)
{
    ZYAN_ASSERT(formatter);
    ZYAN_ASSERT(buffer);
    ZYAN_ASSERT(context);

#if defined(ZYDIS_DISABLE_AVX512) && defined(ZYDIS_DISABLE_KNC)
    ZYAN_UNUSED(formatter);
    ZYAN_UNUSED(buffer);
    ZYAN_UNUSED(context);
#endif

    switch (decorator)
    {
    case ZYDIS_DECORATOR_MASK:
    {
#if !defined(ZYDIS_DISABLE_AVX512) || !defined(ZYDIS_DISABLE_KNC)
        if (context->instruction->avx.mask.reg != ZYDIS_REGISTER_K0)
        {
            if (buffer->is_token_list)
            {
                ZYDIS_BUFFER_APPEND(buffer, DECO_BEGIN);
                ZYAN_CHECK(formatter->func_print_register(formatter, buffer, context,
                    context->instruction->avx.mask.reg));
                ZYDIS_BUFFER_APPEND(buffer, DECO_END);
            } else
            {
                ZYAN_CHECK(ZydisStringAppendShort(&buffer->string, &STR_DECO_BEGIN));
                ZYAN_CHECK(formatter->func_print_register(formatter, buffer, context,
                    context->instruction->avx.mask.reg));
                ZYAN_CHECK(ZydisStringAppendShort(&buffer->string, &STR_DECO_END));
            }

            // Only print the zeroing decorator, if the instruction is not a "zeroing masking only"
            // instruction (e.g. `vcmpsd`)
            if ((context->instruction->avx.mask.mode == ZYDIS_MASK_MODE_ZEROING ||
                 context->instruction->avx.mask.mode == ZYDIS_MASK_MODE_CONTROL_ZEROING) &&
                (context->instruction->raw.evex.z))
            {
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_ZERO, formatter->case_decorators);
            }
        }
#endif
        break;
    }
    case ZYDIS_DECORATOR_BC:
#if !defined(ZYDIS_DISABLE_AVX512)
        if (!context->instruction->avx.broadcast.is_static)
        {
            switch (context->instruction->avx.broadcast.mode)
            {
            case ZYDIS_BROADCAST_MODE_INVALID:
                break;
            case ZYDIS_BROADCAST_MODE_1_TO_2:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_1TO2, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_1_TO_4:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_1TO4, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_1_TO_8:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_1TO8, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_1_TO_16:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_1TO16, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_1_TO_32:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_1TO32, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_1_TO_64:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_1TO64, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_4_TO_8:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_4TO8, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_4_TO_16:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_4TO16, formatter->case_decorators);
                break;
            case ZYDIS_BROADCAST_MODE_8_TO_16:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_8TO16, formatter->case_decorators);
                break;
            default:
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
        }
#endif
        break;
    case ZYDIS_DECORATOR_RC:
#if !defined(ZYDIS_DISABLE_AVX512)
        if (context->instruction->avx.has_sae)
        {
            switch (context->instruction->avx.rounding.mode)
            {
            case ZYDIS_ROUNDING_MODE_INVALID:
                break;
            case ZYDIS_ROUNDING_MODE_RN:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RN_SAE, formatter->case_decorators);
                break;
            case ZYDIS_ROUNDING_MODE_RD:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RD_SAE, formatter->case_decorators);
                break;
            case ZYDIS_ROUNDING_MODE_RU:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RU_SAE, formatter->case_decorators);
                break;
            case ZYDIS_ROUNDING_MODE_RZ:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RZ_SAE, formatter->case_decorators);
                break;
            default:
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
        } else
        {
            switch (context->instruction->avx.rounding.mode)
            {
            case ZYDIS_ROUNDING_MODE_INVALID:
                break;
            case ZYDIS_ROUNDING_MODE_RN:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RN, formatter->case_decorators);
                break;
            case ZYDIS_ROUNDING_MODE_RD:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RD, formatter->case_decorators);
                break;
            case ZYDIS_ROUNDING_MODE_RU:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RU, formatter->case_decorators);
                break;
            case ZYDIS_ROUNDING_MODE_RZ:
                ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_RZ, formatter->case_decorators);
                break;
            default:
                return ZYAN_STATUS_INVALID_ARGUMENT;
            }
        }
#endif
        break;
    case ZYDIS_DECORATOR_SAE:
#if !defined(ZYDIS_DISABLE_AVX512)
        if (context->instruction->avx.has_sae && !context->instruction->avx.rounding.mode)
        {
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_SAE, formatter->case_decorators);
        }
#endif
        break;
    case ZYDIS_DECORATOR_SWIZZLE:
#if !defined(ZYDIS_DISABLE_KNC)
        switch (context->instruction->avx.swizzle.mode)
        {
        case ZYDIS_SWIZZLE_MODE_INVALID:
        case ZYDIS_SWIZZLE_MODE_DCBA:
            // Nothing to do here
            break;
        case ZYDIS_SWIZZLE_MODE_CDAB:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_CDAB, formatter->case_decorators);
            break;
        case ZYDIS_SWIZZLE_MODE_BADC:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_BADC, formatter->case_decorators);
            break;
        case ZYDIS_SWIZZLE_MODE_DACB:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_DACB, formatter->case_decorators);
            break;
        case ZYDIS_SWIZZLE_MODE_AAAA:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_AAAA, formatter->case_decorators);
            break;
        case ZYDIS_SWIZZLE_MODE_BBBB:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_BBBB, formatter->case_decorators);
            break;
        case ZYDIS_SWIZZLE_MODE_CCCC:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_CCCC, formatter->case_decorators);
            break;
        case ZYDIS_SWIZZLE_MODE_DDDD:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_DDDD, formatter->case_decorators);
            break;
        default:
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
#endif
        break;
    case ZYDIS_DECORATOR_CONVERSION:
#if !defined(ZYDIS_DISABLE_KNC)
        switch (context->instruction->avx.conversion.mode)
        {
        case ZYDIS_CONVERSION_MODE_INVALID:
            break;
        case ZYDIS_CONVERSION_MODE_FLOAT16:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_FLOAT16, formatter->case_decorators);
            break;
        case ZYDIS_CONVERSION_MODE_SINT8:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_SINT8, formatter->case_decorators);
            break;
        case ZYDIS_CONVERSION_MODE_UINT8:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_UINT8, formatter->case_decorators);
            break;
        case ZYDIS_CONVERSION_MODE_SINT16:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_SINT16, formatter->case_decorators);
            break;
        case ZYDIS_CONVERSION_MODE_UINT16:
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_UINT16, formatter->case_decorators);
            break;
        default:
            return ZYAN_STATUS_INVALID_ARGUMENT;
        }
#endif
        break;
    case ZYDIS_DECORATOR_EH:
#if !defined(ZYDIS_DISABLE_KNC)
        if (context->instruction->avx.has_eviction_hint)
        {
            ZYDIS_BUFFER_APPEND_CASE(buffer, DECO_EH, formatter->case_decorators);
        }
#endif
        break;
    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
