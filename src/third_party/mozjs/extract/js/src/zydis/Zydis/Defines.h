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

/**
 * @file
 * Import/export defines for MSVC builds.
 */

#ifndef ZYDIS_DEFINES_H
#define ZYDIS_DEFINES_H

#include "zydis/Zycore/Defines.h"

// This is a cut-down version of what CMake's `GenerateExportHeader` would usually generate. To
// simplify builds without CMake, we define these things manually instead of relying on CMake
// to generate the header.
//
// For static builds, our CMakeList will define `ZYDIS_STATIC_BUILD`. For shared library builds,
// our CMake will define `ZYDIS_SHOULD_EXPORT` depending on whether the target is being imported or
// exported. If CMake isn't used, users can manually define these to fit their use-case.

// Backward compatibility: CMake would previously generate these variables names. However, because
// they have pretty cryptic names, we renamed them when we got rid of `GenerateExportHeader`. For
// backward compatibility for users that don't use CMake and previously manually defined these, we
// translate the old defines here and print a warning.
#if defined(ZYDIS_STATIC_DEFINE)
#   pragma message("ZYDIS_STATIC_DEFINE was renamed to ZYDIS_STATIC_BUILD.")
#   define ZYDIS_STATIC_BUILD
#endif
#if defined(Zydis_EXPORTS)
#   pragma message("Zydis_EXPORTS was renamed to ZYDIS_SHOULD_EXPORT.")
#   define ZYDIS_SHOULD_EXPORT
#endif

/**
 * Symbol is exported in shared library builds.
 */
#if defined(ZYDIS_STATIC_BUILD)
#   define ZYDIS_EXPORT
#else
#   if defined(ZYDIS_SHOULD_EXPORT)
#       define ZYDIS_EXPORT ZYAN_DLLEXPORT
#   else
#       define ZYDIS_EXPORT ZYAN_DLLIMPORT
#   endif
#endif

/**
 * Symbol is not exported and for internal use only.
 */
#define ZYDIS_NO_EXPORT

#endif // ZYDIS_DEFINES_H
