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

#include "zydis/Zydis/Internal/SharedData.h"

/* ============================================================================================== */
/* Data tables                                                                                    */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Instruction definitions                                                                        */
/* ---------------------------------------------------------------------------------------------- */

#ifdef ZYDIS_MINIMAL_MODE
#   define ZYDIS_NOTMIN(x)
#else
#   define ZYDIS_NOTMIN(x) , x
#endif

#include "zydis/Zydis/Generated/InstructionDefinitions.inc"

#undef ZYDIS_NOTMIN

/* ---------------------------------------------------------------------------------------------- */
/* Operand definitions                                                                            */
/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_OPERAND_DEFINITION(type, encoding, access) \
    { type, encoding, access }

#include "zydis/Zydis/Generated/OperandDefinitions.inc"

#undef ZYDIS_OPERAND_DEFINITION

/* ---------------------------------------------------------------------------------------------- */
/* Accessed CPU flags                                                                             */
/* ---------------------------------------------------------------------------------------------- */

#include "zydis/Zydis/Generated/AccessedFlags.inc"

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Functions                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Instruction definition                                                                         */
/* ---------------------------------------------------------------------------------------------- */

void ZydisGetInstructionDefinition(ZydisInstructionEncoding encoding, ZyanU16 id,
    const ZydisInstructionDefinition** definition)
{
    switch (encoding)
    {
    case ZYDIS_INSTRUCTION_ENCODING_LEGACY:
        *definition = (ZydisInstructionDefinition*)&ISTR_DEFINITIONS_LEGACY[id];
        break;
    case ZYDIS_INSTRUCTION_ENCODING_3DNOW:
        *definition = (ZydisInstructionDefinition*)&ISTR_DEFINITIONS_3DNOW[id];
        break;
    case ZYDIS_INSTRUCTION_ENCODING_XOP:
        *definition = (ZydisInstructionDefinition*)&ISTR_DEFINITIONS_XOP[id];
        break;
    case ZYDIS_INSTRUCTION_ENCODING_VEX:
        *definition = (ZydisInstructionDefinition*)&ISTR_DEFINITIONS_VEX[id];
        break;
#ifndef ZYDIS_DISABLE_AVX512
    case ZYDIS_INSTRUCTION_ENCODING_EVEX:
        *definition = (ZydisInstructionDefinition*)&ISTR_DEFINITIONS_EVEX[id];
        break;
#endif
#ifndef ZYDIS_DISABLE_KNC
    case ZYDIS_INSTRUCTION_ENCODING_MVEX:
        *definition = (ZydisInstructionDefinition*)&ISTR_DEFINITIONS_MVEX[id];
        break;
#endif
    default:
        ZYAN_UNREACHABLE;
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Operand definition                                                                             */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
const ZydisOperandDefinition* ZydisGetOperandDefinitions(
    const ZydisInstructionDefinition* definition)
{
    if (definition->operand_count == 0)
    {
        return ZYAN_NULL;
    }
    ZYAN_ASSERT(definition->operand_reference != 0xFFFF);
    return &OPERAND_DEFINITIONS[definition->operand_reference];
}
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Element info                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
void ZydisGetElementInfo(ZydisInternalElementType element, ZydisElementType* type,
    ZydisElementSize* size)
{
    static const struct
    {
        ZydisElementType type;
        ZydisElementSize size;
    } lookup[ZYDIS_IELEMENT_TYPE_MAX_VALUE + 1] =
    {
        { ZYDIS_ELEMENT_TYPE_INVALID  ,   0 },
        { ZYDIS_ELEMENT_TYPE_INVALID  ,   0 },
        { ZYDIS_ELEMENT_TYPE_STRUCT   ,   0 },
        { ZYDIS_ELEMENT_TYPE_INT      ,   0 },
        { ZYDIS_ELEMENT_TYPE_UINT     ,   0 },
        { ZYDIS_ELEMENT_TYPE_INT      ,   1 },
        { ZYDIS_ELEMENT_TYPE_INT      ,   8 },
        { ZYDIS_ELEMENT_TYPE_INT      ,  16 },
        { ZYDIS_ELEMENT_TYPE_INT      ,  32 },
        { ZYDIS_ELEMENT_TYPE_INT      ,  64 },
        { ZYDIS_ELEMENT_TYPE_UINT     ,   8 },
        { ZYDIS_ELEMENT_TYPE_UINT     ,  16 },
        { ZYDIS_ELEMENT_TYPE_UINT     ,  32 },
        { ZYDIS_ELEMENT_TYPE_UINT     ,  64 },
        { ZYDIS_ELEMENT_TYPE_UINT     , 128 },
        { ZYDIS_ELEMENT_TYPE_UINT     , 256 },
        { ZYDIS_ELEMENT_TYPE_FLOAT16  ,  16 },
        { ZYDIS_ELEMENT_TYPE_FLOAT16  ,  32 }, // TODO: Should indicate 2 float16 elements
        { ZYDIS_ELEMENT_TYPE_FLOAT32  ,  32 },
        { ZYDIS_ELEMENT_TYPE_FLOAT64  ,  64 },
        { ZYDIS_ELEMENT_TYPE_FLOAT80  ,  80 },
        { ZYDIS_ELEMENT_TYPE_LONGBCD  ,  80 },
        { ZYDIS_ELEMENT_TYPE_CC       ,   3 },
        { ZYDIS_ELEMENT_TYPE_CC       ,   5 }
    };

    ZYAN_ASSERT((ZyanUSize)element < ZYAN_ARRAY_LENGTH(lookup));

    *type = lookup[element].type;
    *size = lookup[element].size;
}
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Accessed CPU flags                                                                             */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYDIS_MINIMAL_MODE
ZyanBool ZydisGetAccessedFlags(const ZydisInstructionDefinition* definition,
    const ZydisDefinitionAccessedFlags** flags)
{
    ZYAN_ASSERT(definition->flags_reference < ZYAN_ARRAY_LENGTH(ACCESSED_FLAGS));
    *flags = &ACCESSED_FLAGS[definition->flags_reference];
    return (definition->flags_reference != 0);
}
#endif

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
