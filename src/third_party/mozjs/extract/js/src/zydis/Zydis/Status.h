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
 * @brief   Status code definitions and check macros.
 */

#ifndef ZYDIS_STATUS_H
#define ZYDIS_STATUS_H

#include "zydis/Zycore/Status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Status codes                                                                                   */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Module IDs                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   The zydis module id.
 */
#define ZYAN_MODULE_ZYDIS   0x002

/* ---------------------------------------------------------------------------------------------- */
/* Status codes                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------------------------- */
/* Decoder                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   An attempt was made to read data from an input data-source that has no more data
 *          available.
 */
#define ZYDIS_STATUS_NO_MORE_DATA \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x00)

/**
 * @brief   An general error occured while decoding the current instruction. The instruction
 *          might be undefined.
 */
#define ZYDIS_STATUS_DECODING_ERROR \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x01)

/**
 * @brief   The instruction exceeded the maximum length of 15 bytes.
 */
#define ZYDIS_STATUS_INSTRUCTION_TOO_LONG \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x02)

/**
 * @brief   The instruction encoded an invalid register.
 */
#define ZYDIS_STATUS_BAD_REGISTER \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x03)

/**
 * @brief   A lock-prefix (F0) was found while decoding an instruction that does not support
 *          locking.
 */
#define ZYDIS_STATUS_ILLEGAL_LOCK \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x04)

/**
 * @brief   A legacy-prefix (F2, F3, 66) was found while decoding a XOP/VEX/EVEX/MVEX
 *          instruction.
 */
#define ZYDIS_STATUS_ILLEGAL_LEGACY_PFX \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x05)

/**
 * @brief   A rex-prefix was found while decoding a XOP/VEX/EVEX/MVEX instruction.
 */
#define ZYDIS_STATUS_ILLEGAL_REX \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x06)

/**
 * @brief   An invalid opcode-map value was found while decoding a XOP/VEX/EVEX/MVEX-prefix.
 */
#define ZYDIS_STATUS_INVALID_MAP \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x07)

/**
 * @brief   An error occured while decoding the EVEX-prefix.
 */
#define ZYDIS_STATUS_MALFORMED_EVEX \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x08)

/**
 * @brief   An error occured while decoding the MVEX-prefix.
 */
#define ZYDIS_STATUS_MALFORMED_MVEX \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x09)

/**
 * @brief   An invalid write-mask was specified for an EVEX/MVEX instruction.
 */
#define ZYDIS_STATUS_INVALID_MASK \
    ZYAN_MAKE_STATUS(1, ZYAN_MODULE_ZYDIS, 0x0A)

/* ---------------------------------------------------------------------------------------------- */
/* Formatter                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returning this status code in some specified formatter callbacks will cause the
 *          formatter to omit the corresponding token.
 *
 * Valid callbacks:
 * - `ZYDIS_FORMATTER_FUNC_PRE_OPERAND`
 * - `ZYDIS_FORMATTER_FUNC_POST_OPERAND`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_REG`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_MEM`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_PTR`
 * - `ZYDIS_FORMATTER_FUNC_FORMAT_OPERAND_IMM`
 */
#define ZYDIS_STATUS_SKIP_TOKEN \
    ZYAN_MAKE_STATUS(0, ZYAN_MODULE_ZYDIS, 0x0B)

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYDIS_STATUS_H */
