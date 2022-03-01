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
 * @brief   Implements a string type.
 */

#ifndef ZYCORE_STRING_H
#define ZYCORE_STRING_H

#include "zydis/ZycoreExportConfig.h"
#include "zydis/Zycore/Allocator.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"
#include "zydis/Zycore/Vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

/**
 * @brief   The initial minimum capacity (number of characters) for all dynamically allocated
 *          string instances - not including the terminating '\0'-character.
 */
#define ZYAN_STRING_MIN_CAPACITY                32

/**
 * @brief   The default growth factor for all string instances.
 */
#define ZYAN_STRING_DEFAULT_GROWTH_FACTOR       2.00f

/**
 * @brief   The default shrink threshold for all string instances.
 */
#define ZYAN_STRING_DEFAULT_SHRINK_THRESHOLD    0.25f

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* String flags                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZyanStringFlags` datatype.
 */
typedef ZyanU8 ZyanStringFlags;

/**
 * @brief   The string uses a custom user-defined buffer with a fixed capacity.
 */
#define ZYAN_STRING_HAS_FIXED_CAPACITY  0x01 // (1 << 0)

/* ---------------------------------------------------------------------------------------------- */
/* String                                                                                         */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZyanString` struct.
 *
 * The `ZyanString` type is implemented as a size-prefixed string - which allows for a lot of
 * performance optimizations.
 * Nevertheless null-termination is guaranteed at all times to provide maximum compatibility with
 * default C-style strings (use `ZyanStringGetData` to access the C-style string).
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanString_
{
    /**
     * @brief   String flags.
     */
    ZyanStringFlags flags;
    /**
     * @brief   The vector that contains the actual string.
     */
    ZyanVector vector;
} ZyanString;

/* ---------------------------------------------------------------------------------------------- */
/* View                                                                                           */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines the `ZyanStringView` struct.
 *
 * The `ZyanStringView` type provides a view inside a string (`ZyanString` instances, null-
 * terminated C-style strings, or even not-null-terminated custom strings). A view is immutable
 * by design and can't be directly converted to a C-style string.
 *
 * Views might become invalid (e.g. pointing to invalid memory), if the underlying string gets
 * destroyed or resized.
 *
 * The `ZYAN_STRING_TO_VIEW` macro can be used to cast a `ZyanString` to a `ZyanStringView` pointer
 * without any runtime overhead.
 * Casting a view to a normal string is not supported and will lead to unexpected behavior (use
 * `ZyanStringDuplicate` to create a deep-copy instead).
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanStringView_
{
    /**
     * @brief   The string data.
     *
     * The view internally re-uses the normal string struct to allow casts without any runtime
     * overhead.
     */
    ZyanString string;
} ZyanStringView;

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* General                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Defines an uninitialized `ZyanString` instance.
 */
#define ZYAN_STRING_INITIALIZER \
    { \
        /* flags  */ 0, \
        /* vector */ ZYAN_VECTOR_INITIALIZER \
    }

/* ---------------------------------------------------------------------------------------------- */
/* Helper macros                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Casts a `ZyanString` pointer to a constant `ZyanStringView` pointer.
 */
#define ZYAN_STRING_TO_VIEW(string) (const ZyanStringView*)(string)

/**
 * @brief   Defines a `ZyanStringView` struct that provides a view into a static C-style string.
 *
 * @param   string  The C-style string.
 */
#define ZYAN_DEFINE_STRING_VIEW(string) \
    { \
        /* string */ \
        { \
            /* flags  */ 0, \
            /* vector */ \
            { \
                /* allocator        */ ZYAN_NULL, \
                /* growth_factor    */ 1.0f, \
                /* shrink_threshold */ 0.0f, \
                /* size             */ sizeof(string), \
                /* capacity         */ sizeof(string), \
                /* element_size     */ sizeof(char), \
                /* destructor       */ ZYAN_NULL, \
                /* data             */ (char*)(string) \
            } \
        } \
    }

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constructor and destructor                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * @brief   Initializes the given `ZyanString` instance.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   capacity        The initial capacity (number of characters).
 *
 * @return  A zyan status code.
 *
 * The memory for the string is dynamically allocated by the default allocator using the default
 * growth factor of `2.0f` and the default shrink threshold of `0.25f`.
 *
 * The allocated buffer will be at least one character larger than the given `capacity`, to reserve
 * space for the terminating '\0'.
 *
 * Finalization with `ZyanStringDestroy` is required for all strings created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanStringInit(ZyanString* string, ZyanUSize capacity);

#endif // ZYAN_NO_LIBC

/**
 * @brief   Initializes the given `ZyanString` instance and sets a custom `allocator` and memory
 *          allocation/deallocation parameters.
 *
 * @param   string              A pointer to the `ZyanString` instance.
 * @param   capacity            The initial capacity (number of characters).
 * @param   allocator           A pointer to a `ZyanAllocator` instance.
 * @param   growth_factor       The growth factor (from `1.0f` to `x.xf`).
 * @param   shrink_threshold    The shrink threshold (from `0.0f` to `1.0f`).
 *
 * @return  A zyan status code.
 *
 * A growth factor of `1.0f` disables overallocation and a shrink threshold of `0.0f` disables
 * dynamic shrinking.
 *
 * The allocated buffer will be at least one character larger than the given `capacity`, to reserve
 * space for the terminating '\0'.
 *
 * Finalization with `ZyanStringDestroy` is required for all strings created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInitEx(ZyanString* string, ZyanUSize capacity,
    ZyanAllocator* allocator, float growth_factor, float shrink_threshold);

/**
 * @brief   Initializes the given `ZyanString` instance and configures it to use a custom user
 *          defined buffer with a fixed size.
 *
 * @param   string          A pointer to the `ZyanString` instance.
 * @param   buffer          A pointer to the buffer that is used as storage for the string.
 * @param   capacity        The maximum capacity (number of characters) of the buffer, including
 *                          the terminating '\0'.
 *
 * @return  A zyan status code.
 *
 * Finalization is not required for strings created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInitCustomBuffer(ZyanString* string, char* buffer,
    ZyanUSize capacity);

/**
 * @brief   Destroys the given `ZyanString` instance.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 *
 * @return  A zyan status code.
 *
 */
ZYCORE_EXPORT ZyanStatus ZyanStringDestroy(ZyanString* string);

/* ---------------------------------------------------------------------------------------------- */
/* Duplication                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * @brief   Initializes a new `ZyanString` instance by duplicating an existing string.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanString` instance.
 * @param   source      A pointer to the source string.
 * @param   capacity    The initial capacity (number of characters).
 *
 *                      This value is automatically adjusted to the size of the source string, if
 *                      a smaller value was passed.
 *
 * @return  A zyan status code.
 *
 * The behavior of this function is undefined, if `source` is a view into the `destination`
 * string or `destination` points to an already initialized `ZyanString` instance.
 *
 * The memory for the string is dynamically allocated by the default allocator using the default
 * growth factor of `2.0f` and the default shrink threshold of `0.25f`.
 *
 * The allocated buffer will be at least one character larger than the given `capacity`, to reserve
 * space for the terminating '\0'.
 *
 * Finalization with `ZyanStringDestroy` is required for all strings created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanStringDuplicate(ZyanString* destination,
    const ZyanStringView* source, ZyanUSize capacity);

#endif // ZYAN_NO_LIBC

/**
 * @brief   Initializes a new `ZyanString` instance by duplicating an existing string and sets a
 *          custom `allocator` and memory allocation/deallocation parameters.
 *
 * @param   destination         A pointer to the (uninitialized) destination `ZyanString` instance.
 * @param   source              A pointer to the source string.
 * @param   capacity            The initial capacity (number of characters).

 *                              This value is automatically adjusted to the size of the source
 *                              string, if a smaller value was passed.
 * @param   allocator           A pointer to a `ZyanAllocator` instance.
 * @param   growth_factor       The growth factor (from `1.0f` to `x.xf`).
 * @param   shrink_threshold    The shrink threshold (from `0.0f` to `1.0f`).
 *
 * @return  A zyan status code.
 *
 * The behavior of this function is undefined, if `source` is a view into the `destination`
 * string or `destination` points to an already initialized `ZyanString` instance.
 *
 * A growth factor of `1.0f` disables overallocation and a shrink threshold of `0.0f` disables
 * dynamic shrinking.
 *
 * The allocated buffer will be at least one character larger than the given `capacity`, to reserve
 * space for the terminating '\0'.
 *
 * Finalization with `ZyanStringDestroy` is required for all strings created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringDuplicateEx(ZyanString* destination,
    const ZyanStringView* source, ZyanUSize capacity, ZyanAllocator* allocator,
    float growth_factor, float shrink_threshold);

/**
 * @brief   Initializes a new `ZyanString` instance by duplicating an existing string and
 *          configures it to use a custom user defined buffer with a fixed size.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanString` instance.
 * @param   source      A pointer to the source string.
 * @param   buffer      A pointer to the buffer that is used as storage for the string.
 * @param   capacity    The maximum capacity (number of characters) of the buffer, including the
 *                      terminating '\0'.

 *                      This function will fail, if the capacity of the buffer is less or equal to
 *                      the size of the source string.
 *
 * @return  A zyan status code.
 *
 * The behavior of this function is undefined, if `source` is a view into the `destination`
 * string or `destination` points to an already initialized `ZyanString` instance.
 *
 * Finalization is not required for strings created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringDuplicateCustomBuffer(ZyanString* destination,
    const ZyanStringView* source, char* buffer, ZyanUSize capacity);

/* ---------------------------------------------------------------------------------------------- */
/* Concatenation                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * @brief   Initializes a new `ZyanString` instance by concatenating two existing strings.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanString` instance.
 *
 *                      This function will fail, if the destination `ZyanString` instance equals
 *                      one of the source strings.
 * @param   s1          A pointer to the first source string.
 * @param   s2          A pointer to the second source string.
 * @param   capacity    The initial capacity (number of characters).

 *                      This value is automatically adjusted to the combined size of the source
 *                      strings, if a smaller value was passed.
 *
 * @return  A zyan status code.
 *
 * The behavior of this function is undefined, if `s1` or `s2` are views into the `destination`
 * string or `destination` points to an already initialized `ZyanString` instance.
 *
 * The memory for the string is dynamically allocated by the default allocator using the default
 * growth factor of `2.0f` and the default shrink threshold of `0.25f`.
 *
 * The allocated buffer will be at least one character larger than the given `capacity`, to reserve
 * space for the terminating '\0'.
 *
 * Finalization with `ZyanStringDestroy` is required for all strings created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanStringConcat(ZyanString* destination,
    const ZyanStringView* s1, const ZyanStringView* s2, ZyanUSize capacity);

#endif // ZYAN_NO_LIBC

/**
 * @brief   Initializes a new `ZyanString` instance by concatenating two existing strings and sets
 *          a custom `allocator` and memory allocation/deallocation parameters.
 *
 * @param   destination         A pointer to the (uninitialized) destination `ZyanString` instance.
 *
 *                              This function will fail, if the destination `ZyanString` instance
 *                              equals one of the source strings.
 * @param   s1                  A pointer to the first source string.
 * @param   s2                  A pointer to the second source string.
 * @param   capacity            The initial capacity (number of characters).
 *
 *                              This value is automatically adjusted to the combined size of the
 *                              source strings, if a smaller value was passed.
 * @param   allocator           A pointer to a `ZyanAllocator` instance.
 * @param   growth_factor       The growth factor (from `1.0f` to `x.xf`).
 * @param   shrink_threshold    The shrink threshold (from `0.0f` to `1.0f`).
 *
 * @return  A zyan status code.
 *
 * The behavior of this function is undefined, if `s1` or `s2` are views into the `destination`
 * string or `destination` points to an already initialized `ZyanString` instance.
 *
 * A growth factor of `1.0f` disables overallocation and a shrink threshold of `0.0f` disables
 * dynamic shrinking.
 *
 * The allocated buffer will be at least one character larger than the given `capacity`, to reserve
 * space for the terminating '\0'.
 *
 * Finalization with `ZyanStringDestroy` is required for all strings created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringConcatEx(ZyanString* destination, const ZyanStringView* s1,
    const ZyanStringView* s2, ZyanUSize capacity, ZyanAllocator* allocator, float growth_factor,
    float shrink_threshold);

/**
 * @brief   Initializes a new `ZyanString` instance by concatenating two existing strings and
 *          configures it to use a custom user defined buffer with a fixed size.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanString` instance.
 *
 *                      This function will fail, if the destination `ZyanString` instance equals
 *                      one of the source strings.
 * @param   s1          A pointer to the first source string.
 * @param   s2          A pointer to the second source string.
 * @param   buffer      A pointer to the buffer that is used as storage for the string.
 * @param   capacity    The maximum capacity (number of characters) of the buffer.
 *
 *                      This function will fail, if the capacity of the buffer is less or equal to
 *                      the combined size of the source strings.
 *
 * @return  A zyan status code.
 *
 * The behavior of this function is undefined, if `s1` or `s2` are views into the `destination`
 * string or `destination` points to an already initialized `ZyanString` instance.
 *
 * Finalization is not required for strings created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringConcatCustomBuffer(ZyanString* destination,
    const ZyanStringView* s1, const ZyanStringView* s2, char* buffer, ZyanUSize capacity);

/* ---------------------------------------------------------------------------------------------- */
/* Views                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns a view inside an existing view/string.
 *
 * @param   view    A pointer to the `ZyanStringView` instance.
 * @param   source  A pointer to the source string.
 *
 * @return  A zyan status code.
 *
 * The `ZYAN_STRING_TO_VEW` macro can be used to pass any `ZyanString` instance as value for the
 * `source` string.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringViewInsideView(ZyanStringView* view,
    const ZyanStringView* source);

/**
 * @brief   Returns a view inside an existing view/string starting from the given `index`.
 *
 * @param   view    A pointer to the `ZyanStringView` instance.
 * @param   source  A pointer to the source string.
 * @param   index   The start index.
 * @param   count   The number of characters.
 *
 * @return  A zyan status code.
 *
 * The `ZYAN_STRING_TO_VEW` macro can be used to pass any `ZyanString` instance as value for the
 * `source` string.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringViewInsideViewEx(ZyanStringView* view,
    const ZyanStringView* source, ZyanUSize index, ZyanUSize count);

/**
 * @brief   Returns a view inside a null-terminated C-style string.
 *
 * @param   view    A pointer to the `ZyanStringView` instance.
 * @param   string  The C-style string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringViewInsideBuffer(ZyanStringView* view, const char* string);

/**
 * @brief   Returns a view inside a character buffer with custom length.
 *
 * @param   view    A pointer to the `ZyanStringView` instance.
 * @param   buffer  A pointer to the buffer containing the string characters.
 * @param   length  The length of the string (number of characters).
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringViewInsideBufferEx(ZyanStringView* view, const char* buffer,
    ZyanUSize length);

/**
 * @brief   Returns the size (number of characters) of the view.
 *
 * @param   view    A pointer to the `ZyanStringView` instance.
 * @param   size    Receives the size (number of characters) of the view.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringViewGetSize(const ZyanStringView* view, ZyanUSize* size);

/**
 * @brief   Returns the C-style string of the given `ZyanString` instance.
 *
 * @warning The string is not guaranteed to be null terminated!
 *
 * @param   string  A pointer to the `ZyanStringView` instance.
 * @param   value   Receives a pointer to the C-style string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringViewGetData(const ZyanStringView* view, const char** buffer);

/* ---------------------------------------------------------------------------------------------- */
/* Character access                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns the character at the given `index`.
 *
 * @param   string  A pointer to the `ZyanStringView` instance.
 * @param   index   The character index.
 * @param   value   Receives the desired character of the string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringGetChar(const ZyanStringView* string, ZyanUSize index,
    char* value);

/**
 * @brief   Returns a pointer to the character at the given `index`.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   index   The character index.
 * @param   value   Receives a pointer to the desired character in the string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringGetCharMutable(ZyanString* string, ZyanUSize index,
    char** value);

/**
 * @brief   Assigns a new value to the character at the given `index`.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   index   The character index.
 * @param   value   The character to assign.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringSetChar(ZyanString* string, ZyanUSize index, char value);

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Inserts the content of the source string in the destination string at the given `index`.
 *
 * @param   destination The destination string.
 * @param   index       The insert index.
 * @param   source      The source string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInsert(ZyanString* destination, ZyanUSize index,
    const ZyanStringView* source);

/**
 * @brief   Inserts `count` characters of the source string in the destination string at the given
 *          `index`.
 *
 * @param   destination         The destination string.
 * @param   destination_index   The insert index.
 * @param   source              The source string.
 * @param   source_index        The index of the first character to be inserted from the source
 *                              string.
 * @param   count               The number of chars to insert from the source string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringInsertEx(ZyanString* destination, ZyanUSize destination_index,
    const ZyanStringView* source, ZyanUSize source_index, ZyanUSize count);

/* ---------------------------------------------------------------------------------------------- */
/* Appending                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Appends the content of the source string to the end of the destination string.
 *
 * @param   destination The destination string.
 * @param   source      The source string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringAppend(ZyanString* destination, const ZyanStringView* source);

/**
 * @brief   Appends `count` characters of the source string to the end of the destination string.
 *
 * @param   destination     The destination string.
 * @param   source          The source string.
 * @param   source_index    The index of the first character to be appended from the source string.
 * @param   count           The number of chars to append from the source string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringAppendEx(ZyanString* destination, const ZyanStringView* source,
    ZyanUSize source_index, ZyanUSize count);

/* ---------------------------------------------------------------------------------------------- */
/* Deletion                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Deletes characters from the given string, starting at `index`.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   index   The index of the first character to delete.
 * @param   count   The number of characters to delete.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringDelete(ZyanString* string, ZyanUSize index, ZyanUSize count);

/**
 * @brief   Deletes all remaining characters from the given string, starting at `index`.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   index   The index of the first character to delete.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringTruncate(ZyanString* string, ZyanUSize index);

/**
 * @brief   Erases the given string.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringClear(ZyanString* string);

/* ---------------------------------------------------------------------------------------------- */
/* Searching                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Searches for the first occurrence of `needle` in the given `haystack` starting from the
 *          left.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringLPos(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index);

/**
 * @brief   Searches for the first occurrence of `needle` in the given `haystack` starting from the
 *          left.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 * @param   index       The start index.
 * @param   count       The maximum number of characters to iterate, beginning from the start
 *                      `index`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringLPosEx(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index, ZyanUSize index, ZyanUSize count);

/**
 * @brief   Performs a case-insensitive search for the first occurrence of `needle` in the given
 *          `haystack` starting from the left.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringLPosI(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index);

/**
 * @brief   Performs a case-insensitive search for the first occurrence of `needle` in the given
 *          `haystack` starting from the left.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 * @param   index       The start index.
 * @param   count       The maximum number of characters to iterate, beginning from the start
 *                      `index`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringLPosIEx(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index, ZyanUSize index, ZyanUSize count);

/**
 * @brief   Searches for the first occurrence of `needle` in the given `haystack` starting from the
 *          right.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringRPos(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index);

/**
 * @brief   Searches for the first occurrence of `needle` in the given `haystack` starting from the
 *          right.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 * @param   index       The start index.
 * @param   count       The maximum number of characters to iterate, beginning from the start
 *                      `index`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringRPosEx(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index, ZyanUSize index, ZyanUSize count);

/**
 * @brief   Performs a case-insensitive search for the first occurrence of `needle` in the given
 *          `haystack` starting from the right.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringRPosI(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index);

/**
 * @brief   Performs a case-insensitive search for the first occurrence of `needle` in the given
 *          `haystack` starting from the right.
 *
 * @param   haystack    The string to search in.
 * @param   needle      The sub-string to search for.
 * @param   found_index A pointer to a variable that receives the index of the first occurrence of
 *                      `needle`.
 * @param   index       The start index.
 * @param   count       The maximum number of characters to iterate, beginning from the start
 *                      `index`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the needle was found, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 *
 * The `found_index` is set to `-1`, if the needle was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringRPosIEx(const ZyanStringView* haystack,
    const ZyanStringView* needle, ZyanISize* found_index, ZyanUSize index, ZyanUSize count);

/* ---------------------------------------------------------------------------------------------- */
/* Comparing                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Compares two strings.
 *
 * @param   s1      The first string
 * @param   s2      The second string.
 * @param   result  Receives the comparison result.
 *
 *                  Values:
 *                  - `result  < 0` -> The first character that does not match has a lower value
 *                    in `s1` than in `s2`.
 *                  - `result == 0` -> The contents of both strings are equal.
 *                  - `result  > 0` -> The first character that does not match has a greater value
 *                    in `s1` than in `s2`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the strings are equal, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringCompare(const ZyanStringView* s1, const ZyanStringView* s2,
    ZyanI32* result);

/**
 * @brief   Performs a case-insensitive comparison of two strings.
 *
 * @param   s1      The first string
 * @param   s2      The second string.
 * @param   result  Receives the comparison result.
 *
 *                  Values:
 *                  - `result  < 0` -> The first character that does not match has a lower value
 *                    in `s1` than in `s2`.
 *                  - `result == 0` -> The contents of both strings are equal.
 *                  - `result  > 0` -> The first character that does not match has a greater value
 *                    in `s1` than in `s2`.
 *
 * @return  `ZYAN_STATUS_TRUE`, if the strings are equal, `ZYAN_STATUS_FALSE`, if not, or another
 *          zyan status code, if an error occured.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringCompareI(const ZyanStringView* s1, const ZyanStringView* s2,
    ZyanI32* result);

/* ---------------------------------------------------------------------------------------------- */
/* Case conversion                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Converts the given string to lowercase letters.
 *
 * @param   string      A pointer to the `ZyanString` instance.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringToLowerCase(ZyanString* string);

/**
 * @brief   Converts `count` characters of the given string to lowercase letters.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   index   The start index.
 * @param   count   The number of characters to convert, beginning from the start `index`.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringToLowerCaseEx(ZyanString* string, ZyanUSize index,
    ZyanUSize count);

/**
 * @brief   Converts the given string to uppercase letters.
 *
 * @param   string      A pointer to the `ZyanString` instance.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringToUpperCase(ZyanString* string);

/**
 * @brief   Converts `count` characters of the given string to uppercase letters.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   index   The start index.
 * @param   count   The number of characters to convert, beginning from the start `index`.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringToUpperCaseEx(ZyanString* string, ZyanUSize index,
    ZyanUSize count);

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Resizes the given `ZyanString` instance.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   size    The new size of the string.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringResize(ZyanString* string, ZyanUSize size);

/**
 * @brief   Changes the capacity of the given `ZyanString` instance.
 *
 * @param   string      A pointer to the `ZyanString` instance.
 * @param   capacity    The new minimum capacity of the string.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringReserve(ZyanString* string, ZyanUSize capacity);

/**
 * @brief   Shrinks the capacity of the given string to match it's size.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 *
 * @return  A zyan status code.
 *
 * This function will fail, if the `ZYAN_STRING_IS_IMMUTABLE` flag is set for the specified
 * `ZyanString` instance.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringShrinkToFit(ZyanString* string);

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * @brief   Returns the current capacity of the string.
 *
 * @param   string      A pointer to the `ZyanString` instance.
 * @param   capacity    Receives the size of the string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringGetCapacity(const ZyanString* string, ZyanUSize* capacity);

/**
 * @brief   Returns the current size (number of characters) of the string (excluding the
 *          terminating zero character).
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   size    Receives the size (number of characters) of the string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringGetSize(const ZyanString* string, ZyanUSize* size);

/**
 * @brief   Returns the C-style string of the given `ZyanString` instance.
 *
 * @param   string  A pointer to the `ZyanString` instance.
 * @param   value   Receives a pointer to the C-style string.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanStringGetData(const ZyanString* string, const char** value);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif // ZYCORE_STRING_H
