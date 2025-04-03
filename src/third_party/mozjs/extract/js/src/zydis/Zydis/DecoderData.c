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

#include "zydis/Zydis/Internal/DecoderData.h"

/* ============================================================================================== */
/* Data tables                                                                                    */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Physical instruction encodings                                                                 */
/* ---------------------------------------------------------------------------------------------- */

#include "zydis/Zydis/Generated/InstructionEncodings.inc"

/* ---------------------------------------------------------------------------------------------- */
/* Decoder tree                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

#define ZYDIS_INVALID \
    { ZYDIS_NODETYPE_INVALID, 0x00000000 }
#define ZYDIS_FILTER(type, id) \
    { type, id }
#define ZYDIS_DEFINITION(encoding_id, id) \
    { ZYDIS_NODETYPE_DEFINITION_MASK | encoding_id, id }

#include "zydis/Zydis/Generated/DecoderTables.inc"

#undef ZYDIS_INVALID
#undef ZYDIS_FILTER
#undef ZYDIS_DEFINITION

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Functions                                                                                      */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Decoder tree                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

const ZydisDecoderTreeNode zydis_decoder_tree_root = { ZYDIS_NODETYPE_FILTER_OPCODE, 0x0000 };

const ZydisDecoderTreeNode* ZydisDecoderTreeGetChildNode(const ZydisDecoderTreeNode* parent,
    ZyanU16 index)
{
    switch (parent->type)
    {
    case ZYDIS_NODETYPE_FILTER_XOP:
        ZYAN_ASSERT(index <  13);
        return &FILTERS_XOP[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_VEX:
        ZYAN_ASSERT(index <  17);
        return &FILTERS_VEX[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_EMVEX:
        ZYAN_ASSERT(index <  49);
        return &FILTERS_EMVEX[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_OPCODE:
        ZYAN_ASSERT(index < 256);
        return &FILTERS_OPCODE[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE:
        ZYAN_ASSERT(index <   4);
        return &FILTERS_MODE[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_COMPACT:
        ZYAN_ASSERT(index <   3);
        return &FILTERS_MODE_COMPACT[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODRM_MOD:
        ZYAN_ASSERT(index <   4);
        return &FILTERS_MODRM_MOD[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODRM_MOD_COMPACT:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODRM_MOD_COMPACT[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODRM_REG:
        ZYAN_ASSERT(index <   8);
        return &FILTERS_MODRM_REG[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODRM_RM:
        ZYAN_ASSERT(index <   8);
        return &FILTERS_MODRM_RM[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_PREFIX_GROUP1:
        ZYAN_ASSERT(index < 2);
        return &FILTERS_PREFIX_GROUP1[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MANDATORY_PREFIX:
        ZYAN_ASSERT(index <   5);
        return &FILTERS_MANDATORY_PREFIX[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_OPERAND_SIZE:
        ZYAN_ASSERT(index <   3);
        return &FILTERS_OPERAND_SIZE[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_ADDRESS_SIZE:
        ZYAN_ASSERT(index <   3);
        return &FILTERS_ADDRESS_SIZE[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_VECTOR_LENGTH:
        ZYAN_ASSERT(index <   3);
        return &FILTERS_VECTOR_LENGTH[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_REX_W:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_REX_W[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_REX_B:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_REX_B[parent->value][index];
#ifndef ZYDIS_DISABLE_AVX512
    case ZYDIS_NODETYPE_FILTER_EVEX_B:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_EVEX_B[parent->value][index];
#endif
#ifndef ZYDIS_DISABLE_KNC
    case ZYDIS_NODETYPE_FILTER_MVEX_E:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MVEX_E[parent->value][index];
#endif
    case ZYDIS_NODETYPE_FILTER_MODE_AMD:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_AMD[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_KNC:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_KNC[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_MPX:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_MPX[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_CET:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_CET[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_LZCNT:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_LZCNT[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_TZCNT:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_TZCNT[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_WBNOINVD:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_WBNOINVD[parent->value][index];
    case ZYDIS_NODETYPE_FILTER_MODE_CLDEMOTE:
        ZYAN_ASSERT(index <   2);
        return &FILTERS_MODE_CLDEMOTE[parent->value][index];
    default:
        ZYAN_UNREACHABLE;
    }
}

void ZydisGetInstructionEncodingInfo(const ZydisDecoderTreeNode* node,
    const ZydisInstructionEncodingInfo** info)
{
    ZYAN_ASSERT(node->type & ZYDIS_NODETYPE_DEFINITION_MASK);
    const ZyanU8 class = (node->type) & 0x7F;
    ZYAN_ASSERT(class < ZYAN_ARRAY_LENGTH(INSTR_ENCODINGS));
    *info = &INSTR_ENCODINGS[class];
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
