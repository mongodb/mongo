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
 * @brief
 */

#ifndef ZYDIS_METAINFO_H
#define ZYDIS_METAINFO_H

#include "zydis/ZydisExportConfig.h"
#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

#include "zydis/Zydis/Generated/EnumInstructionCategory.h"
#include "zydis/Zydis/Generated/EnumISASet.h"
#include "zydis/Zydis/Generated/EnumISAExt.h"

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

 /**
 * @brief   Returns the specified instruction category string.
 *
 * @param   category    The instruction category.
 *
 * @return  The instruction category string or `ZYAN_NULL`, if an invalid category was passed.
 */
ZYDIS_EXPORT const char* ZydisCategoryGetString(ZydisInstructionCategory category);

/**
 * @brief   Returns the specified isa-set string.
 *
 * @param   isa_set The isa-set.
 *
 * @return  The isa-set string or `ZYAN_NULL`, if an invalid isa-set was passed.
 */
ZYDIS_EXPORT const char* ZydisISASetGetString(ZydisISASet isa_set);

/**
 * @brief   Returns the specified isa-extension string.
 *
 * @param   isa_ext The isa-extension.
 *
 * @return  The isa-extension string or `ZYAN_NULL`, if an invalid isa-extension was passed.
 */
ZYDIS_EXPORT const char* ZydisISAExtGetString(ZydisISAExt isa_ext);

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_METAINFO_H */
