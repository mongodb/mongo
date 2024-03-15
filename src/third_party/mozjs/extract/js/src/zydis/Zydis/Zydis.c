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

#include "zydis/Zydis/Zydis.h"

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

ZyanU64 ZydisGetVersion(void)
{
    return ZYDIS_VERSION;
}

ZyanStatus ZydisIsFeatureEnabled(ZydisFeature feature)
{
    switch (feature)
    {
    case ZYDIS_FEATURE_DECODER:
#ifndef ZYDIS_DISABLE_DECODER
        return ZYAN_STATUS_TRUE;
#else
        return ZYAN_STATUS_FALSE;
#endif
    case ZYDIS_FEATURE_ENCODER:
#ifndef ZYDIS_DISABLE_ENCODER
        return ZYAN_STATUS_TRUE;
#else
        return ZYAN_STATUS_FALSE;
#endif
    case ZYDIS_FEATURE_FORMATTER:
#ifndef ZYDIS_DISABLE_FORMATTER
        return ZYAN_STATUS_TRUE;
#else
        return ZYAN_STATUS_FALSE;
#endif
    case ZYDIS_FEATURE_AVX512:
#ifndef ZYDIS_DISABLE_AVX512
        return ZYAN_STATUS_TRUE;
#else
        return ZYAN_STATUS_FALSE;
#endif

    case ZYDIS_FEATURE_KNC:
#ifndef ZYDIS_DISABLE_KNC
        return ZYAN_STATUS_TRUE;
#else
        return ZYAN_STATUS_FALSE;
#endif

    case ZYDIS_FEATURE_SEGMENT:
#ifndef ZYDIS_DISABLE_SEGMENT
        return ZYAN_STATUS_TRUE;
#else
        return ZYAN_STATUS_FALSE;
#endif

    default:
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
}

/* ============================================================================================== */
