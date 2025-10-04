/***************************************************************************************************

  Zyan Core Library (Zyan-C)

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

/**
 * @file
 * Status code definitions and check macros.
 */

#ifndef ZYCORE_STATUS_H
#define ZYCORE_STATUS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "zydis/Zycore/Types.h"

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanStatus` data type.
 */
typedef ZyanU32 ZyanStatus;

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Definition                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines a zyan status code.
 *
 * @param   error   `1`, if the status code signals an error or `0`, if not.
 * @param   module  The module id.
 * @param   code    The actual code.
 *
 * @return  The zyan status code.
 */
#define ZYAN_MAKE_STATUS(error, module, code) \
    (ZyanStatus)((((error) & 0x01u) << 31u) | (((module) & 0x7FFu) << 20u) | ((code) & 0xFFFFFu))

/* ---------------------------------------------------------------------------------------------- */
/* Checks                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Checks if a zyan operation was successful.
 *
 * @param   status  The zyan status-code to check.
 *
 * @return  `ZYAN_TRUE`, if the operation succeeded or `ZYAN_FALSE`, if not.
 */
#define ZYAN_SUCCESS(status) \
    (!((status) & 0x80000000u))

/**
 * Checks if a zyan operation failed.
 *
 * @param   status  The zyan status-code to check.
 *
 * @return  `ZYAN_TRUE`, if the operation failed or `ZYAN_FALSE`, if not.
 */
#define ZYAN_FAILED(status) \
    ((status) & 0x80000000u)

/**
 * Checks if a zyan operation was successful and returns with the status-code, if not.
 *
 * @param   status  The zyan status-code to check.
 */
#define ZYAN_CHECK(status) \
    do \
    { \
        const ZyanStatus status_047620348 = (status); \
        if (!ZYAN_SUCCESS(status_047620348)) \
        { \
            return status_047620348; \
        } \
    } while (0)

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

 /**
 * Returns the module id of a zyan status-code.
 *
 * @param   status  The zyan status-code.
 *
 * @return  The module id of the zyan status-code.
 */
#define ZYAN_STATUS_MODULE(status) \
    (((status) >> 20) & 0x7FFu)

 /**
 * Returns the code of a zyan status-code.
 *
 * @param   status  The zyan status-code.
 *
 * @return  The code of the zyan status-code.
 */
#define ZYAN_STATUS_CODE(status) \
    ((status) & 0xFFFFFu)

/* ============================================================================================== */
/* Status codes                                                                                   */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Module IDs                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

/**
 * The zycore generic module id.
 */
#define ZYAN_MODULE_ZYCORE      0x001u

/**
 * The zycore arg-parse submodule id.
 */
#define ZYAN_MODULE_ARGPARSE    0x003u

/**
 * The base module id for user-defined status codes.
 */
#define ZYAN_MODULE_USER        0x3FFu

/* ---------------------------------------------------------------------------------------------- */
/* Status codes (general purpose)                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * The operation completed successfully.
 */
#define ZYAN_STATUS_SUCCESS \
    ZYAN_MAKE_STATUS(0u, ZYAN_MODULE_ZYCORE, 0x00u)

/**
 * The operation failed with an generic error.
 */
#define ZYAN_STATUS_FAILED \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x01u)

/**
 * The operation completed successfully and returned `ZYAN_TRUE`.
 */
#define ZYAN_STATUS_TRUE \
    ZYAN_MAKE_STATUS(0u, ZYAN_MODULE_ZYCORE, 0x02u)

/**
 * The operation completed successfully and returned `ZYAN_FALSE`.
 */
#define ZYAN_STATUS_FALSE \
    ZYAN_MAKE_STATUS(0u, ZYAN_MODULE_ZYCORE, 0x03u)

/**
 * An invalid argument was passed to a function.
 */
#define ZYAN_STATUS_INVALID_ARGUMENT \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x04u)

/**
 * An attempt was made to perform an invalid operation.
 */
#define ZYAN_STATUS_INVALID_OPERATION \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x05u)

/**
 * Insufficient privileges to perform the requested operation.
 */
#define ZYAN_STATUS_ACCESS_DENIED \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x06u)

/**
 * The requested entity was not found.
 */
#define ZYAN_STATUS_NOT_FOUND \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x07u)

/**
 * An index passed to a function was out of bounds.
 */
#define ZYAN_STATUS_OUT_OF_RANGE \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x08u)

/**
 * A buffer passed to a function was too small to complete the requested operation.
 */
#define ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x09u)

/**
 * Insufficient memory to perform the operation.
 */
#define ZYAN_STATUS_NOT_ENOUGH_MEMORY \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x0Au)

/**
 * An unknown error occurred during a system function call.
 */
#define ZYAN_STATUS_BAD_SYSTEMCALL \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x0Bu)

/**
 * The process ran out of resources while performing an operation.
 */
#define ZYAN_STATUS_OUT_OF_RESOURCES \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x0Cu)

/**
 * A dependency library was not found or does have an unexpected version number or
 * feature-set.
 */
#define ZYAN_STATUS_MISSING_DEPENDENCY \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ZYCORE, 0x0Du)

/* ---------------------------------------------------------------------------------------------- */
/* Status codes (arg parse)                                                                       */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Argument was not expected.
 */
#define ZYAN_STATUS_ARG_NOT_UNDERSTOOD \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ARGPARSE, 0x00u)

/**
 * Too few arguments were provided.
 */
#define ZYAN_STATUS_TOO_FEW_ARGS \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ARGPARSE, 0x01u)

/**
 * Too many arguments were provided.
 */
#define ZYAN_STATUS_TOO_MANY_ARGS \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ARGPARSE, 0x02u)

/**
 * An argument that expected a value misses its value.
 */
#define ZYAN_STATUS_ARG_MISSES_VALUE \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ARGPARSE, 0x03u)

/**
* A required argument is missing.
*/
#define ZYAN_STATUS_REQUIRED_ARG_MISSING \
    ZYAN_MAKE_STATUS(1u, ZYAN_MODULE_ARGPARSE, 0x04u)

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_STATUS_H */
