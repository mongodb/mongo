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
 * Mnemonic constant definitions and helper functions.
 */

#ifndef ZYDIS_MNEMONIC_H
#define ZYDIS_MNEMONIC_H

#include "zydis/Zydis/Defines.h"
#include "zydis/Zydis/ShortString.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

#include "zydis/Zydis/Generated/EnumMnemonic.h"

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * @addtogroup mnemonic Mnemonic
 * Functions for retrieving mnemonic names.
 * @{
 */

/**
 * Returns the specified instruction mnemonic string.
 *
 * @param   mnemonic    The mnemonic.
 *
 * @return  The instruction mnemonic string or `ZYAN_NULL`, if an invalid mnemonic was passed.
 */
ZYDIS_EXPORT const char* ZydisMnemonicGetString(ZydisMnemonic mnemonic);

/**
 * Returns the specified instruction mnemonic as `ZydisShortString`.
 *
 * @param   mnemonic    The mnemonic.
 *
 * @return  The instruction mnemonic string or `ZYAN_NULL`, if an invalid mnemonic was passed.
 *
 * The `buffer` of the returned struct is guaranteed to be zero-terminated in this special case.
 */
ZYDIS_EXPORT const ZydisShortString* ZydisMnemonicGetStringWrapped(ZydisMnemonic mnemonic);

/**
 * @}
 */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_MNEMONIC_H */
