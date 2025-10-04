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
 * Defines prototypes of general-purpose comparison functions.
 */

#ifndef ZYCORE_COMPARISON_H
#define ZYCORE_COMPARISON_H

#include "zydis/Zycore/Defines.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanEqualityComparison` function prototype.
 *
 * @param   left    A pointer to the first element.
 * @param   right   A pointer to the second element.
 *
 * @return  This function should return `ZYAN_TRUE` if the `left` element equals the `right` one
 *          or `ZYAN_FALSE`, if not.
 */
typedef ZyanBool (*ZyanEqualityComparison)(const void* left, const void* right);

/**
 * Defines the `ZyanComparison` function prototype.
 *
 * @param   left    A pointer to the first element.
 * @param   right   A pointer to the second element.
 *
 * @return  This function should return values in the following range:
 *          `left == right -> result == 0`
 *          `left <  right -> result  < 0`
 *          `left >  right -> result  > 0`
 */
typedef ZyanI32 (*ZyanComparison)(const void* left, const void* right);

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Equality comparison functions                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Declares a generic equality comparison function for an integral data-type.
 *
 * @param   name    The name of the function.
 * @param   type    The name of the integral data-type.
 */
#define ZYAN_DECLARE_EQUALITY_COMPARISON(name, type) \
    ZyanBool name(const type* left, const type* right) \
    { \
        ZYAN_ASSERT(left); \
        ZYAN_ASSERT(right); \
        \
        return (*left == *right) ? ZYAN_TRUE : ZYAN_FALSE; \
    }

/**
 * Declares a generic equality comparison function that compares a single integral
 *          data-type field of a struct.
 *
 * @param   name        The name of the function.
 * @param   type        The name of the integral data-type.
 * @param   field_name  The name of the struct field.
 */
#define ZYAN_DECLARE_EQUALITY_COMPARISON_FOR_FIELD(name, type, field_name) \
    ZyanBool name(const type* left, const type* right) \
    { \
        ZYAN_ASSERT(left); \
        ZYAN_ASSERT(right); \
        \
        return (left->field_name == right->field_name) ? ZYAN_TRUE : ZYAN_FALSE; \
    }

/* ---------------------------------------------------------------------------------------------- */
/* Comparison functions                                                                           */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Declares a generic comparison function for an integral data-type.
 *
 * @param   name    The name of the function.
 * @param   type    The name of the integral data-type.
 */
#define ZYAN_DECLARE_COMPARISON(name, type) \
    ZyanI32 name(const type* left, const type* right) \
    { \
        ZYAN_ASSERT(left); \
        ZYAN_ASSERT(right); \
        \
        if (*left < *right) \
        { \
            return -1; \
        } \
        if (*left > *right) \
        { \
            return  1; \
        } \
        return 0; \
    }

/**
 * Declares a generic comparison function that compares a single integral data-type field
 *          of a struct.
 *
 * @param   name        The name of the function.
 * @param   type        The name of the integral data-type.
 * @param   field_name  The name of the struct field.
 */
#define ZYAN_DECLARE_COMPARISON_FOR_FIELD(name, type, field_name) \
    ZyanI32 name(const type* left, const type* right) \
    { \
        ZYAN_ASSERT(left); \
        ZYAN_ASSERT(right); \
        \
        if (left->field_name < right->field_name) \
        { \
            return -1; \
        } \
        if (left->field_name > right->field_name) \
        { \
            return  1; \
        } \
        return 0; \
    }

 /* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Default equality comparison functions                                                          */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines a default equality comparison function for pointer values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `ZYAN_TRUE` if the `left` value equals the `right` one or `ZYAN_FALSE`, if
 *          not.
 */
ZYAN_INLINE ZYAN_DECLARE_EQUALITY_COMPARISON(ZyanEqualsPointer, void* const)

/**
 * Defines a default equality comparison function for `ZyanBool` values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `ZYAN_TRUE` if the `left` value equals the `right` one or `ZYAN_FALSE`, if
 *          not.
 */
ZYAN_INLINE ZYAN_DECLARE_EQUALITY_COMPARISON(ZyanEqualsBool, ZyanBool)

/**
 * Defines a default equality comparison function for 8-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `ZYAN_TRUE` if the `left` value equals the `right` one or `ZYAN_FALSE`, if
 *          not.
 */
ZYAN_INLINE ZYAN_DECLARE_EQUALITY_COMPARISON(ZyanEqualsNumeric8, ZyanU8)

/**
 * Defines a default equality comparison function for 16-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `ZYAN_TRUE` if the `left` value equals the `right` one or `ZYAN_FALSE`, if
 *          not.
 */
ZYAN_INLINE ZYAN_DECLARE_EQUALITY_COMPARISON(ZyanEqualsNumeric16, ZyanU16)

/**
 * Defines a default equality comparison function for 32-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `ZYAN_TRUE` if the `left` value equals the `right` one or `ZYAN_FALSE`, if
 *          not.
 */
ZYAN_INLINE ZYAN_DECLARE_EQUALITY_COMPARISON(ZyanEqualsNumeric32, ZyanU32)

/**
 * Defines a default equality comparison function for 64-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `ZYAN_TRUE` if the `left` value equals the `right` one or `ZYAN_FALSE`, if
 *          not.
 */
ZYAN_INLINE ZYAN_DECLARE_EQUALITY_COMPARISON(ZyanEqualsNumeric64, ZyanU64)

/* ---------------------------------------------------------------------------------------------- */
/* Default comparison functions                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines a default comparison function for pointer values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `0` if the `left` value equals the `right` one, `-1` if the `left` value is
 *          less than the `right` one, or `1` if the `left` value is greater than the `right` one.
 */
ZYAN_INLINE ZYAN_DECLARE_COMPARISON(ZyanComparePointer, void* const)

/**
 * Defines a default comparison function for `ZyanBool` values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `0` if the `left` value equals the `right` one, `-1` if the `left` value is
 *          less than the `right` one, or `1` if the `left` value is greater than the `right` one.
 */
ZYAN_INLINE ZYAN_DECLARE_COMPARISON(ZyanCompareBool, ZyanBool)

/**
 * Defines a default comparison function for 8-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `0` if the `left` value equals the `right` one, `-1` if the `left` value is
 *          less than the `right` one, or `1` if the `left` value is greater than the `right` one.
 */
ZYAN_INLINE ZYAN_DECLARE_COMPARISON(ZyanCompareNumeric8, ZyanU8)

/**
 * Defines a default comparison function for 16-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `0` if the `left` value equals the `right` one, `-1` if the `left` value is
 *          less than the `right` one, or `1` if the `left` value is greater than the `right` one.
 */
ZYAN_INLINE ZYAN_DECLARE_COMPARISON(ZyanCompareNumeric16, ZyanU16)

/**
 * Defines a default comparison function for 32-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `0` if the `left` value equals the `right` one, `-1` if the `left` value is
 *          less than the `right` one, or `1` if the `left` value is greater than the `right` one.
 */
ZYAN_INLINE ZYAN_DECLARE_COMPARISON(ZyanCompareNumeric32, ZyanU32)

/**
 * Defines a default comparison function for 64-bit numeric values.
 *
 * @param   left    A pointer to the first value.
 * @param   right   A pointer to the second value.
 *
 * @return  Returns `0` if the `left` value equals the `right` one, `-1` if the `left` value is
 *          less than the `right` one, or `1` if the `left` value is greater than the `right` one.
 */
ZYAN_INLINE ZYAN_DECLARE_COMPARISON(ZyanCompareNumeric64, ZyanU64)

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_COMPARISON_H */
