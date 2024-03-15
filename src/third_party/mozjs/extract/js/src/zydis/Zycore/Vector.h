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
 * Implements the vector container class.
 */

#ifndef ZYCORE_VECTOR_H
#define ZYCORE_VECTOR_H

#include "zydis/Zycore/Allocator.h"
#include "zydis/Zycore/Comparison.h"
#include "zydis/Zycore/Object.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Constants                                                                                      */
/* ============================================================================================== */

/**
 * The initial minimum capacity (number of elements) for all dynamically allocated vector
 * instances.
 */
#define ZYAN_VECTOR_MIN_CAPACITY                1

/**
 * The default growth factor for all vector instances.
 */
#define ZYAN_VECTOR_DEFAULT_GROWTH_FACTOR       2

/**
 * The default shrink threshold for all vector instances.
 */
#define ZYAN_VECTOR_DEFAULT_SHRINK_THRESHOLD    4

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanVector` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanVector_
{
    /**
     * The memory allocator.
     */
    ZyanAllocator* allocator;
    /**
     * The growth factor.
     */
    ZyanU8 growth_factor;
    /**
     * The shrink threshold.
     */
    ZyanU8 shrink_threshold;
    /**
     * The current number of elements in the vector.
     */
    ZyanUSize size;
    /**
     * The maximum capacity (number of elements).
     */
    ZyanUSize capacity;
    /**
     * The size of a single element in bytes.
     */
    ZyanUSize element_size;
    /**
     * The element destructor callback.
     */
    ZyanMemberProcedure destructor;
    /**
     * The data pointer.
     */
    void* data;
} ZyanVector;

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* General                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines an uninitialized `ZyanVector` instance.
 */
#define ZYAN_VECTOR_INITIALIZER \
    { \
        /* allocator        */ ZYAN_NULL, \
        /* growth_factor    */ 0, \
        /* shrink_threshold */ 0, \
        /* size             */ 0, \
        /* capacity         */ 0, \
        /* element_size     */ 0, \
        /* destructor       */ ZYAN_NULL, \
        /* data             */ ZYAN_NULL \
    }

/* ---------------------------------------------------------------------------------------------- */
/* Helper macros                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the value of the element at the given `index`.
 *
 * @param   type    The desired value type.
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The element index.
 *
 * @result  The value of the desired element in the vector.
 *
 * Note that this function is unsafe and might dereference a null-pointer.
 */
#ifdef __cplusplus
#define ZYAN_VECTOR_GET(type, vector, index) \
    (*reinterpret_cast<const type*>(ZyanVectorGet(vector, index)))
#else
#define ZYAN_VECTOR_GET(type, vector, index) \
    (*(const type*)ZyanVectorGet(vector, index))
#endif

/**
 * Loops through all elements of the vector.
 *
 * @param   type        The desired value type.
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   item_name   The name of the iterator item.
 * @param   body        The body to execute for each item in the vector.
 */
#define ZYAN_VECTOR_FOREACH(type, vector, item_name, body) \
    { \
        const ZyanUSize ZYAN_MACRO_CONCAT_EXPAND(size_d50d3303, item_name) = (vector)->size; \
        for (ZyanUSize ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name) = 0; \
            ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name) < \
            ZYAN_MACRO_CONCAT_EXPAND(size_d50d3303, item_name); \
            ++ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name)) \
        { \
            const type item_name = ZYAN_VECTOR_GET(type, vector, \
                ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name)); \
            body \
        } \
    }

/**
 * Loops through all elements of the vector.
 *
 * @param   type        The desired value type.
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   item_name   The name of the iterator item.
 * @param   body        The body to execute for each item in the vector.
 */
#define ZYAN_VECTOR_FOREACH_MUTABLE(type, vector, item_name, body) \
    { \
        const ZyanUSize ZYAN_MACRO_CONCAT_EXPAND(size_d50d3303, item_name) = (vector)->size; \
        for (ZyanUSize ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name) = 0; \
            ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name) < \
            ZYAN_MACRO_CONCAT_EXPAND(size_d50d3303, item_name); \
            ++ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name)) \
        { \
            type* const item_name = ZyanVectorGetMutable(vector, \
                ZYAN_MACRO_CONCAT_EXPAND(i_bfd62679, item_name)); \
            body \
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
 * Initializes the given `ZyanVector` instance.
 *
 * @param   vector          A pointer to the `ZyanVector` instance.
 * @param   element_size    The size of a single element in bytes.
 * @param   capacity        The initial capacity (number of elements).
 * @param   destructor      A destructor callback that is invoked every time an item is deleted, or
 *                          `ZYAN_NULL` if not needed.
 *
 * @return  A zyan status code.
 *
 * The memory for the vector elements is dynamically allocated by the default allocator using the
 * default growth factor and the default shrink threshold.
 *
 * Finalization with `ZyanVectorDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanVectorInit(ZyanVector* vector,
    ZyanUSize element_size, ZyanUSize capacity, ZyanMemberProcedure destructor);

#endif // ZYAN_NO_LIBC

/**
 * Initializes the given `ZyanVector` instance and sets a custom `allocator` and memory
 * allocation/deallocation parameters.
 *
 * @param   vector              A pointer to the `ZyanVector` instance.
 * @param   element_size        The size of a single element in bytes.
 * @param   capacity            The initial capacity (number of elements).
 * @param   destructor          A destructor callback that is invoked every time an item is deleted,
 *                              or `ZYAN_NULL` if not needed.
 * @param   allocator           A pointer to a `ZyanAllocator` instance.
 * @param   growth_factor       The growth factor.
 * @param   shrink_threshold    The shrink threshold.
 *
 * @return  A zyan status code.
 *
 * A growth factor of `1` disables overallocation and a shrink threshold of `0` disables
 * dynamic shrinking.
 *
 * Finalization with `ZyanVectorDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorInitEx(ZyanVector* vector, ZyanUSize element_size,
    ZyanUSize capacity, ZyanMemberProcedure destructor, ZyanAllocator* allocator,
    ZyanU8 growth_factor, ZyanU8 shrink_threshold);

/**
 * Initializes the given `ZyanVector` instance and configures it to use a custom user
 * defined buffer with a fixed size.
 *
 * @param   vector          A pointer to the `ZyanVector` instance.
 * @param   element_size    The size of a single element in bytes.
 * @param   buffer          A pointer to the buffer that is used as storage for the elements.
 * @param   capacity        The maximum capacity (number of elements) of the buffer.
 * @param   destructor      A destructor callback that is invoked every time an item is deleted, or
 *                          `ZYAN_NULL` if not needed.
 *
 * @return  A zyan status code.
 *
 * Finalization is not required for instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorInitCustomBuffer(ZyanVector* vector, ZyanUSize element_size,
    void* buffer, ZyanUSize capacity, ZyanMemberProcedure destructor);

/**
 * Destroys the given `ZyanVector` instance.
 *
 * @param   vector  A pointer to the `ZyanVector` instance..
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorDestroy(ZyanVector* vector);

/* ---------------------------------------------------------------------------------------------- */
/* Duplication                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * Initializes a new `ZyanVector` instance by duplicating an existing vector.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanVector` instance.
 * @param   source      A pointer to the source vector.
 * @param   capacity    The initial capacity (number of elements).
 *
 *                      This value is automatically adjusted to the size of the source vector, if
 *                      a smaller value was passed.
 *
 * @return  A zyan status code.
 *
 * The memory for the vector is dynamically allocated by the default allocator using the default
 * growth factor and the default shrink threshold.
 *
 * Finalization with `ZyanVectorDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanVectorDuplicate(ZyanVector* destination,
    const ZyanVector* source, ZyanUSize capacity);

#endif // ZYAN_NO_LIBC

/**
 * Initializes a new `ZyanVector` instance by duplicating an existing vector and sets a
 * custom `allocator` and memory allocation/deallocation parameters.
 *
 * @param   destination         A pointer to the (uninitialized) destination `ZyanVector` instance.
 * @param   source              A pointer to the source vector.
 * @param   capacity            The initial capacity (number of elements).

 *                              This value is automatically adjusted to the size of the source
 *                              vector, if a smaller value was passed.
 * @param   allocator           A pointer to a `ZyanAllocator` instance.
 * @param   growth_factor       The growth factor.
 * @param   shrink_threshold    The shrink threshold.
 *
 * @return  A zyan status code.
 *
 * A growth factor of `1` disables overallocation and a shrink threshold of `0` disables
 * dynamic shrinking.
 *
 * Finalization with `ZyanVectorDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorDuplicateEx(ZyanVector* destination, const ZyanVector* source,
    ZyanUSize capacity, ZyanAllocator* allocator, ZyanU8 growth_factor, ZyanU8 shrink_threshold);

/**
 * Initializes a new `ZyanVector` instance by duplicating an existing vector and
 * configures it to use a custom user defined buffer with a fixed size.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanVector` instance.
 * @param   source      A pointer to the source vector.
 * @param   buffer      A pointer to the buffer that is used as storage for the elements.
 * @param   capacity    The maximum capacity (number of elements) of the buffer.

 *                      This function will fail, if the capacity of the buffer is less than the
 *                      size of the source vector.
 *
 * @return  A zyan status code.
 *
 * Finalization is not required for instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorDuplicateCustomBuffer(ZyanVector* destination,
    const ZyanVector* source, void* buffer, ZyanUSize capacity);

/* ---------------------------------------------------------------------------------------------- */
/* Element access                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns a constant pointer to the element at the given `index`.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   index       The element index.
 *
 * @return  A constant pointer to the desired element in the vector or `ZYAN_NULL`, if an error
 *          occurred.
 *
 * Note that the returned pointer might get invalid when the vector is resized by either a manual
 * call to the memory-management functions or implicitly by inserting or removing elements.
 *
 * Take a look at `ZyanVectorGetPointer` instead, if you need a function that returns a zyan status
 * code.
 */
ZYCORE_EXPORT const void* ZyanVectorGet(const ZyanVector* vector, ZyanUSize index);

/**
 * Returns a mutable pointer to the element at the given `index`.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   index       The element index.
 *
 * @return  A mutable pointer to the desired element in the vector or `ZYAN_NULL`, if an error
 *          occurred.
 *
 * Note that the returned pointer might get invalid when the vector is resized by either a manual
 * call to the memory-management functions or implicitly by inserting or removing elements.
 *
 * Take a look at `ZyanVectorGetPointerMutable` instead, if you need a function that returns a
 * zyan status code.
 */
ZYCORE_EXPORT void* ZyanVectorGetMutable(const ZyanVector* vector, ZyanUSize index);

/**
 * Returns a constant pointer to the element at the given `index`.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The element index.
 * @param   value   Receives a constant pointer to the desired element in the vector.
 *
 * Note that the returned pointer might get invalid when the vector is resized by either a manual
 * call to the memory-management functions or implicitly by inserting or removing elements.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorGetPointer(const ZyanVector* vector, ZyanUSize index,
    const void** value);

/**
 * Returns a mutable pointer to the element at the given `index`.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The element index.
 * @param   value Receives a mutable pointer to the desired element in the vector.
 *
 * Note that the returned pointer might get invalid when the vector is resized by either a manual
 * call to the memory-management functions or implicitly by inserting or removing elements.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorGetPointerMutable(const ZyanVector* vector, ZyanUSize index,
    void** value);

/**
 * Assigns a new value to the element at the given `index`.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The value index.
 * @param   value   The value to assign.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorSet(ZyanVector* vector, ZyanUSize index,
    const void* value);

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Adds a new `element` to the end of the vector.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   element A pointer to the element to add.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorPushBack(ZyanVector* vector, const void* element);

/**
 * Inserts an `element` at the given `index` of the vector.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The insert index.
 * @param   element A pointer to the element to insert.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorInsert(ZyanVector* vector, ZyanUSize index,
    const void* element);

/**
 * Inserts multiple `elements` at the given `index` of the vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   index       The insert index.
 * @param   elements    A pointer to the first element.
 * @param   count       The number of elements to insert.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorInsertRange(ZyanVector* vector, ZyanUSize index,
    const void* elements, ZyanUSize count);

/**
 * Constructs an `element` in-place at the end of the vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   element     Receives a pointer to the new element.
 * @param   constructor The constructor callback or `ZYAN_NULL`. The new element will be in
 *                      undefined state, if no constructor was passed.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorEmplace(ZyanVector* vector, void** element,
    ZyanMemberFunction constructor);

/**
 * Constructs an `element` in-place and inserts it at the given `index` of the vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   index       The insert index.
 * @param   element     Receives a pointer to the new element.
 * @param   constructor The constructor callback or `ZYAN_NULL`. The new element will be in
 *                      undefined state, if no constructor was passed.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorEmplaceEx(ZyanVector* vector, ZyanUSize index,
    void** element, ZyanMemberFunction constructor);

/* ---------------------------------------------------------------------------------------------- */
/* Utils                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Swaps the element at `index_first` with the element at `index_second`.
 *
 * @param   vector          A pointer to the `ZyanVector` instance.
 * @param   index_first     The index of the first element.
 * @param   index_second    The index of the second element.
 *
 * @return  A zyan status code.
 *
 * This function requires the vector to have spare capacity for one temporary element. Call
 * `ZyanVectorReserve` before this function to increase capacity, if needed.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorSwapElements(ZyanVector* vector, ZyanUSize index_first,
    ZyanUSize index_second);

/* ---------------------------------------------------------------------------------------------- */
/* Deletion                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Deletes the element at the given `index` of the vector.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The element index.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorDelete(ZyanVector* vector, ZyanUSize index);

/**
 * Deletes multiple elements from the given vector, starting at `index`.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   index   The index of the first element to delete.
 * @param   count   The number of elements to delete.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorDeleteRange(ZyanVector* vector, ZyanUSize index,
    ZyanUSize count);

/**
 * Removes the last element of the vector.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorPopBack(ZyanVector* vector);

/**
 * Erases all elements of the given vector.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorClear(ZyanVector* vector);

/* ---------------------------------------------------------------------------------------------- */
/* Searching                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Sequentially searches for the first occurrence of `element` in the given vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   element     A pointer to the element to search for.
 * @param   found_index A pointer to a variable that receives the index of the found element.
 * @param   comparison  The comparison function to use.
 *
 * @return  `ZYAN_STATUS_TRUE` if the element was found, `ZYAN_STATUS_FALSE` if not or a generic
 *          zyan status code if an error occurred.
 *
 * The `found_index` is set to `-1`, if the element was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorFind(const ZyanVector* vector, const void* element,
    ZyanISize* found_index, ZyanEqualityComparison comparison);

/**
 * Sequentially searches for the first occurrence of `element` in the given vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   element     A pointer to the element to search for.
 * @param   found_index A pointer to a variable that receives the index of the found element.
 * @param   comparison  The comparison function to use.
 * @param   index       The start index.
 * @param   count       The maximum number of elements to iterate, beginning from the start `index`.
 *
 * @return  `ZYAN_STATUS_TRUE` if the element was found, `ZYAN_STATUS_FALSE` if not or a generic
 *          zyan status code if an error occurred.
 *
 * The `found_index` is set to `-1`, if the element was not found.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorFindEx(const ZyanVector* vector, const void* element,
    ZyanISize* found_index, ZyanEqualityComparison comparison, ZyanUSize index, ZyanUSize count);

/**
 * Searches for the first occurrence of `element` in the given vector using a binary-
 * search algorithm.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   element     A pointer to the element to search for.
 * @param   found_index A pointer to a variable that receives the index of the found element.
 * @param   comparison  The comparison function to use.
 *
 * @return  `ZYAN_STATUS_TRUE` if the element was found, `ZYAN_STATUS_FALSE` if not or a generic
 *          zyan status code if an error occurred.
 *
 * If found, `found_index` contains the zero-based index of `element`. If not found, `found_index`
 * contains the index of the first entry larger than `element`.
 *
 * This function requires all elements in the vector to be strictly ordered (sorted).
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorBinarySearch(const ZyanVector* vector, const void* element,
    ZyanUSize* found_index, ZyanComparison comparison);

/**
 * Searches for the first occurrence of `element` in the given vector using a binary-
 * search algorithm.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   element     A pointer to the element to search for.
 * @param   found_index A pointer to a variable that receives the index of the found element.
 * @param   comparison  The comparison function to use.
 * @param   index       The start index.
 * @param   count       The maximum number of elements to iterate, beginning from the start `index`.
 *
 * @return  `ZYAN_STATUS_TRUE` if the element was found, `ZYAN_STATUS_FALSE` if not or a generic
 *          zyan status code if an error occurred.
 *
 * If found, `found_index` contains the zero-based index of `element`. If not found, `found_index`
 * contains the index of the first entry larger than `element`.
 *
 * This function requires all elements in the vector to be strictly ordered (sorted).
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorBinarySearchEx(const ZyanVector* vector, const void* element,
    ZyanUSize* found_index, ZyanComparison comparison, ZyanUSize index, ZyanUSize count);

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Resizes the given `ZyanVector` instance.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   size    The new size of the vector.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorResize(ZyanVector* vector, ZyanUSize size);

/**
 * Resizes the given `ZyanVector` instance.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   size        The new size of the vector.
 * @param   initializer A pointer to a value to be used as initializer for new items.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorResizeEx(ZyanVector* vector, ZyanUSize size,
    const void* initializer);

/**
 * Changes the capacity of the given `ZyanVector` instance.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   capacity    The new minimum capacity of the vector.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorReserve(ZyanVector* vector, ZyanUSize capacity);

/**
 * Shrinks the capacity of the given vector to match it's size.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorShrinkToFit(ZyanVector* vector);

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the current capacity of the vector.
 *
 * @param   vector      A pointer to the `ZyanVector` instance.
 * @param   capacity    Receives the size of the vector.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorGetCapacity(const ZyanVector* vector, ZyanUSize* capacity);

/**
 * Returns the current size of the vector.
 *
 * @param   vector  A pointer to the `ZyanVector` instance.
 * @param   size    Receives the size of the vector.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanVectorGetSize(const ZyanVector* vector, ZyanUSize* size);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_VECTOR_H */
