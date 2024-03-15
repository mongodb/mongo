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
 * Implements a doubly linked list.
 */

#ifndef ZYCORE_LIST_H
#define ZYCORE_LIST_H

#include "zydis/Zycore/Allocator.h"
#include "zydis/Zycore/Object.h"
#include "zydis/Zycore/Status.h"
#include "zydis/Zycore/Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================================== */
/* Enums and types                                                                                */
/* ============================================================================================== */

/**
 * Defines the `ZyanListNode` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanListNode_
{
    /**
     * A pointer to the previous list node.
     */
    struct ZyanListNode_* prev;
    /**
     * A pointer to the next list node.
     */
    struct ZyanListNode_* next;
} ZyanListNode;

/**
 * Defines the `ZyanList` struct.
 *
 * All fields in this struct should be considered as "private". Any changes may lead to unexpected
 * behavior.
 */
typedef struct ZyanList_
{
    /**
     * The memory allocator.
     */
    ZyanAllocator* allocator;
    /**
     * The current number of elements in the list.
     */
    ZyanUSize size;
    /**
     * The size of a single element in bytes.
     */
    ZyanUSize element_size;
    /**
     * The element destructor callback.
     */
    ZyanMemberProcedure destructor;
    /**
     * The head node.
     */
    ZyanListNode* head;
    /**
     * The tail node.
     */
    ZyanListNode* tail;
    /**
     * The data buffer.
     *
     * Only used for instances created by `ZyanListInitCustomBuffer`.
     */
    void* buffer;
    /**
     * The data buffer capacity (number of bytes).
     *
     * Only used for instances created by `ZyanListInitCustomBuffer`.
     */
    ZyanUSize capacity;
    /**
     * The first unused node.
     *
     * When removing a node, the first-unused value is updated to point at the removed node and the
     * next node of the removed node will be updated to point at the old first-unused node.
     *
     * When appending the memory of the first unused-node is recycled to store the new node. The
     * value of the first-unused node is then updated to point at the reused nodes next node.
     *
     * If the first-unused value is `ZYAN_NULL`, any new node will be "allocated" behind the tail
     * node (if there is enough space left in the fixed size buffer).
     *
     * Only used for instances created by `ZyanListInitCustomBuffer`.
     */
    ZyanListNode* first_unused;
} ZyanList;

/* ============================================================================================== */
/* Macros                                                                                         */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* General                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Defines an uninitialized `ZyanList` instance.
 */
#define ZYAN_LIST_INITIALIZER \
    { \
        /* allocator        */ ZYAN_NULL, \
        /* size             */ 0, \
        /* element_size     */ 0, \
        /* head             */ ZYAN_NULL, \
        /* destructor       */ ZYAN_NULL, \
        /* tail             */ ZYAN_NULL, \
        /* buffer           */ ZYAN_NULL, \
        /* capacity         */ 0, \
        /* first_unused     */ ZYAN_NULL \
    }

/* ---------------------------------------------------------------------------------------------- */
/* Helper macros                                                                                  */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the data value of the given `node`.
 *
 * @param   type    The desired value type.
 * @param   node    A pointer to the `ZyanListNode` struct.
 *
 * @result  The data value of the given `node`.
 *
 * Note that this function is unsafe and might dereference a null-pointer.
 */
#ifdef __cplusplus
#define ZYAN_LIST_GET(type, node) \
    (*reinterpret_cast<const type*>(ZyanListGetNodeData(node)))
#else
#define ZYAN_LIST_GET(type, node) \
    (*(const type*)ZyanListGetNodeData(node))
#endif

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constructor and destructor                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * Initializes the given `ZyanList` instance.
 *
 * @param   list            A pointer to the `ZyanList` instance.
 * @param   element_size    The size of a single element in bytes.
 * @param   destructor      A destructor callback that is invoked every time an item is deleted, or
 *                          `ZYAN_NULL` if not needed.
 *
 * @return  A zyan status code.
 *
 * The memory for the list elements is dynamically allocated by the default allocator.
 *
 * Finalization with `ZyanListDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanListInit(ZyanList* list, ZyanUSize element_size,
    ZyanMemberProcedure destructor);

#endif // ZYAN_NO_LIBC

/**
 * Initializes the given `ZyanList` instance and sets a custom `allocator`.
 *
 * @param   list            A pointer to the `ZyanList` instance.
 * @param   element_size    The size of a single element in bytes.
 * @param   destructor      A destructor callback that is invoked every time an item is deleted, or
 *                          `ZYAN_NULL` if not needed.
 * @param   allocator       A pointer to a `ZyanAllocator` instance.
 *
 * @return  A zyan status code.
 *
 * Finalization with `ZyanListDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanListInitEx(ZyanList* list, ZyanUSize element_size,
    ZyanMemberProcedure destructor, ZyanAllocator* allocator);

/**
 * Initializes the given `ZyanList` instance and configures it to use a custom user
 * defined buffer with a fixed size.
 *
 * @param   list            A pointer to the `ZyanList` instance.
 * @param   element_size    The size of a single element in bytes.
 * @param   destructor      A destructor callback that is invoked every time an item is deleted, or
 *                          `ZYAN_NULL` if not needed.
 * @param   buffer          A pointer to the buffer that is used as storage for the elements.
 * @param   capacity        The maximum capacity (number of bytes) of the buffer including the
 *                          space required for the list-nodes.
 *
 * @return  A zyan status code.
 *
 * The buffer capacity required to store `n` elements of type `T` is be calculated by:
 * `size = n * sizeof(ZyanListNode) + n * sizeof(T)`
 *
 * Finalization is not required for instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanListInitCustomBuffer(ZyanList* list, ZyanUSize element_size,
    ZyanMemberProcedure destructor, void* buffer, ZyanUSize capacity);

/**
 * Destroys the given `ZyanList` instance.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListDestroy(ZyanList* list);

/* ---------------------------------------------------------------------------------------------- */
/* Duplication                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

/**
 * Initializes a new `ZyanList` instance by duplicating an existing list.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanList` instance.
 * @param   source      A pointer to the source list.
 *
 * @return  A zyan status code.
 *
 * The memory for the list is dynamically allocated by the default allocator.
 *
 * Finalization with `ZyanListDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZYAN_REQUIRES_LIBC ZyanStatus ZyanListDuplicate(ZyanList* destination,
    const ZyanList* source);

#endif // ZYAN_NO_LIBC

/**
 * Initializes a new `ZyanList` instance by duplicating an existing list and sets a
 * custom `allocator`.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanList` instance.
 * @param   source      A pointer to the source list.
 * @param   allocator   A pointer to a `ZyanAllocator` instance.
 *
 * @return  A zyan status code.

 * Finalization with `ZyanListDestroy` is required for all instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanListDuplicateEx(ZyanList* destination, const ZyanList* source,
    ZyanAllocator* allocator);

/**
 * Initializes a new `ZyanList` instance by duplicating an existing list and
 * configures it to use a custom user defined buffer with a fixed size.
 *
 * @param   destination A pointer to the (uninitialized) destination `ZyanList` instance.
 * @param   source      A pointer to the source list.
 * @param   buffer      A pointer to the buffer that is used as storage for the elements.
 * @param   capacity    The maximum capacity (number of bytes) of the buffer including the
 *                      space required for the list-nodes.

 *                      This function will fail, if the capacity of the buffer is not sufficient
 *                      to store all elements of the source list.
 *
 * @return  A zyan status code.
 *
 * The buffer capacity required to store `n` elements of type `T` is be calculated by:
 * `size = n * sizeof(ZyanListNode) + n * sizeof(T)`
 *
 * Finalization is not required for instances created by this function.
 */
ZYCORE_EXPORT ZyanStatus ZyanListDuplicateCustomBuffer(ZyanList* destination,
    const ZyanList* source, void* buffer, ZyanUSize capacity);

/* ---------------------------------------------------------------------------------------------- */
/* Item access                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns a pointer to the first `ZyanListNode` struct of the given list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   node    Receives a pointer to the first `ZyanListNode` struct of the list.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetHeadNode(const ZyanList* list, const ZyanListNode** node);

/**
 * Returns a pointer to the last `ZyanListNode` struct of the given list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   node    Receives a pointer to the last `ZyanListNode` struct of the list.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetTailNode(const ZyanList* list, const ZyanListNode** node);

/**
 * Receives a pointer to the previous `ZyanListNode` struct linked to the passed one.
 *
 * @param   node    Receives a pointer to the previous `ZyanListNode` struct linked to the passed
 *                  one.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetPrevNode(const ZyanListNode** node);

/**
 * Receives a pointer to the next `ZyanListNode` struct linked to the passed one.
 *
 * @param   node    Receives a pointer to the next `ZyanListNode` struct linked to the passed one.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetNextNode(const ZyanListNode** node);

/**
 * Returns a constant pointer to the data of the given `node`.
 *
 * @param   node    A pointer to the `ZyanListNode` struct.
 *
 * @return  A constant pointer to the the data of the given `node` or `ZYAN_NULL`, if an error
 *          occured.
 *
 * Take a look at `ZyanListGetNodeDataEx`, if you need a function that returns a zyan status code.
 */
ZYCORE_EXPORT const void* ZyanListGetNodeData(const ZyanListNode* node);

/**
 * Returns a constant pointer to the data of the given `node`..
 *
 * @param   node    A pointer to the `ZyanListNode` struct.
 * @param   value   Receives a constant pointer to the data of the given `node`.
 *
 * Take a look at `ZyanListGetNodeData`, if you need a function that directly returns a pointer.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetNodeDataEx(const ZyanListNode* node, const void** value);

/**
 * Returns a mutable pointer to the data of the given `node`.
 *
 * @param   node    A pointer to the `ZyanListNode` struct.
 *
 * @return  A mutable pointer to the the data of the given `node` or `ZYAN_NULL`, if an error
 *          occured.
 *
 * Take a look at `ZyanListGetPointerMutableEx` instead, if you need a function that returns a
 * zyan status code.
 */
ZYCORE_EXPORT void* ZyanListGetNodeDataMutable(const ZyanListNode* node);

/**
 * Returns a mutable pointer to the data of the given `node`..
 *
 * @param   node    A pointer to the `ZyanListNode` struct.
 * @param   value   Receives a mutable pointer to the data of the given `node`.
 *
 * Take a look at `ZyanListGetNodeDataMutable`, if you need a function that directly returns a
 * pointer.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetNodeDataMutableEx(const ZyanListNode* node, void** value);

/**
 * Assigns a new data value to the given `node`.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   node    A pointer to the `ZyanListNode` struct.
 * @param   value   The value to assign.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListSetNodeData(const ZyanList* list, const ZyanListNode* node,
    const void* value);

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Adds a new `item` to the end of the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   item    A pointer to the item to add.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListPushBack(ZyanList* list, const void* item);

/**
 * Adds a new `item` to the beginning of the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   item    A pointer to the item to add.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListPushFront(ZyanList* list, const void* item);

/**
 * Constructs an `item` in-place at the end of the list.
 *
 * @param   list        A pointer to the `ZyanList` instance.
 * @param   item        Receives a pointer to the new item.
 * @param   constructor The constructor callback or `ZYAN_NULL`. The new item will be in
 *                      undefined state, if no constructor was passed.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListEmplaceBack(ZyanList* list, void** item,
    ZyanMemberFunction constructor);

/**
 * Constructs an `item` in-place at the beginning of the list.
 *
 * @param   list        A pointer to the `ZyanList` instance.
 * @param   item        Receives a pointer to the new item.
 * @param   constructor The constructor callback or `ZYAN_NULL`. The new item will be in
 *                      undefined state, if no constructor was passed.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListEmplaceFront(ZyanList* list, void** item,
    ZyanMemberFunction constructor);

/* ---------------------------------------------------------------------------------------------- */
/* Deletion                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Removes the last element of the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListPopBack(ZyanList* list);

/**
 * Removes the firstelement of the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListPopFront(ZyanList* list);

/**
 * Removes the given `node` from the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   node    A pointer to the `ZyanListNode` struct.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListRemove(ZyanList* list, const ZyanListNode* node);

/**
 * Removes multiple nodes from the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   first   A pointer to the first node.
 * @param   last    A pointer to the last node.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListRemoveRange(ZyanList* list, const ZyanListNode* first,
    const ZyanListNode* last);

/**
 * Erases all elements of the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListClear(ZyanList* list);

/* ---------------------------------------------------------------------------------------------- */
/* Searching                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

// TODO:

/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Resizes the given `ZyanList` instance.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   size    The new size of the list.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListResize(ZyanList* list, ZyanUSize size);

/**
 * Resizes the given `ZyanList` instance.
 *
 * @param   list        A pointer to the `ZyanList` instance.
 * @param   size        The new size of the list.
 * @param   initializer A pointer to a value to be used as initializer for new items.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListResizeEx(ZyanList* list, ZyanUSize size, const void* initializer);

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Returns the current size of the list.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   size    Receives the size of the list.
 *
 * @return  A zyan status code.
 */
ZYCORE_EXPORT ZyanStatus ZyanListGetSize(const ZyanList* list, ZyanUSize* size);

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */

#ifdef __cplusplus
}
#endif

#endif /* ZYCORE_VECTOR_H */
