/***************************************************************************************************

  Zyan Core Library (Zycore-C)

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
 * Master include file, including everything else.
 */

#ifndef ZYCORE_H
#define ZYCORE_H

#include "zydis/Zycore/Types.h"

// TODO:

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constants                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * A macro that defines the zycore version.
 */
#define ZYCORE_VERSION (ZyanU64)0x0001000400010000

/* ---------------------------------------------------------------------------------------------- */
/* Helper macros                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Extracts the major-part of the zycore version.
 *
 * @param   version The zycore version value
 */
#define ZYCORE_VERSION_MAJOR(version) (ZyanU16)((version & 0xFFFF000000000000) >> 48)

/**
 * Extracts the minor-part of the zycore version.
 *
 * @param   version The zycore version value
 */
#define ZYCORE_VERSION_MINOR(version) (ZyanU16)((version & 0x0000FFFF00000000) >> 32)

/**
 * Extracts the patch-part of the zycore version.
 *
 * @param   version The zycore version value
 */
#define ZYCORE_VERSION_PATCH(version) (ZyanU16)((version & 0x00000000FFFF0000) >> 16)

/**
 * Extracts the build-part of the zycore version.
 *
 * @param   version The zycore version value
 */
#define ZYCORE_VERSION_BUILD(version) (ZyanU16)(version & 0x000000000000FFFF)

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/**
 * Returns the zycore version.
 *
 * @return  The zycore version.
 *
 * Use the macros provided in this file to extract the major, minor, patch and build part from the
 * returned version value.
 */
ZYCORE_EXPORT ZyanU64 ZycoreGetVersion(void);

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_H */
