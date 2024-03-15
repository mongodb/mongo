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

#include "zydis/Zycore/LibC.h"
#include "zydis/Zycore/List.h"

/* ============================================================================================== */
/* Internal macros                                                                                */
/* ============================================================================================== */

/**
 * Returns a pointer to the data of the given `node`.
 *
 * @param   node    A pointer to the `ZyanNodeData` struct.
 *
 * @return  A pointer to the data of the given `node`.
 */
#define ZYCORE_LIST_GET_NODE_DATA(node) \
    ((void*)(node + 1))

/* ============================================================================================== */
/* Internal functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Helper functions                                                                               */
/* ---------------------------------------------------------------------------------------------- */

/**
 * Allocates memory for a new list node.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   node    Receives a pointer to the new `ZyanListNode` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZyanListAllocateNode(ZyanList* list, ZyanListNode** node)
{
    ZYAN_ASSERT(list);
    ZYAN_ASSERT(node);

    const ZyanBool is_dynamic = (list->allocator != ZYAN_NULL);
    if (is_dynamic)
    {
        ZYAN_ASSERT(list->allocator->allocate);
        ZYAN_CHECK(list->allocator->allocate(list->allocator, (void**)node,
            sizeof(ZyanListNode) + list->element_size, 1));
    } else
    {
        if (list->first_unused)
        {
            *node = list->first_unused;
            list->first_unused = (*node)->next;
        } else
        {
            const ZyanUSize size = list->size * (sizeof(ZyanListNode) + list->element_size);
            if (size + (sizeof(ZyanListNode) + list->element_size) > list->capacity)
            {
                return ZYAN_STATUS_INSUFFICIENT_BUFFER_SIZE;
            }

            *node = (ZyanListNode*)((ZyanU8*)list->buffer + size);
        }
    }

    return ZYAN_STATUS_SUCCESS;
}

/**
 * Frees memory of a node.
 *
 * @param   list    A pointer to the `ZyanList` instance.
 * @param   node    A pointer to the `ZyanListNode` struct.
 *
 * @return  A zyan status code.
 */
static ZyanStatus ZyanListDeallocateNode(ZyanList* list, ZyanListNode* node)
{
    ZYAN_ASSERT(list);
    ZYAN_ASSERT(node);

    const ZyanBool is_dynamic = (list->allocator != ZYAN_NULL);
    if (is_dynamic)
    {
        ZYAN_ASSERT(list->allocator->deallocate);
        ZYAN_CHECK(list->allocator->deallocate(list->allocator, (void*)node,
            sizeof(ZyanListNode) + list->element_size, 1));
    } else
    {
        node->next = list->first_unused;
        list->first_unused = node;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
/* Exported functions                                                                             */
/* ============================================================================================== */

/* ---------------------------------------------------------------------------------------------- */
/* Constructor and destructor                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#ifndef ZYAN_NO_LIBC

ZYAN_REQUIRES_LIBC ZyanStatus ZyanListInit(ZyanList* list, ZyanUSize element_size,
    ZyanMemberProcedure destructor)
{
    return ZyanListInitEx(list, element_size, destructor, ZyanAllocatorDefault());
}

#endif // ZYAN_NO_LIBC

ZyanStatus ZyanListInitEx(ZyanList* list, ZyanUSize element_size, ZyanMemberProcedure destructor,
    ZyanAllocator* allocator)
{
    if (!list || !element_size || !allocator)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    list->allocator     = allocator;
    list->size          = 0;
    list->element_size  = element_size;
    list->destructor    = destructor;
    list->head          = ZYAN_NULL;
    list->tail          = ZYAN_NULL;
    list->buffer        = ZYAN_NULL;
    list->capacity      = 0;
    list->first_unused  = ZYAN_NULL;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListInitCustomBuffer(ZyanList* list, ZyanUSize element_size,
    ZyanMemberProcedure destructor, void* buffer, ZyanUSize capacity)
{
    if (!list || !element_size || !buffer || !capacity)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    list->allocator    = ZYAN_NULL;
    list->size         = 0;
    list->element_size = element_size;
    list->destructor   = destructor;
    list->head         = ZYAN_NULL;
    list->tail         = ZYAN_NULL;
    list->buffer       = buffer;
    list->capacity     = capacity;
    list->first_unused = ZYAN_NULL;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListDestroy(ZyanList* list)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZYAN_ASSERT(list->element_size);

    const ZyanBool is_dynamic = (list->allocator != ZYAN_NULL);
    ZyanListNode* node = (is_dynamic || list->destructor) ? list->head : ZYAN_NULL;
    while (node)
    {
        if (list->destructor)
        {
            list->destructor(ZYCORE_LIST_GET_NODE_DATA(node));
        }

        ZyanListNode* const next = node->next;

        if (is_dynamic)
        {
            ZYAN_CHECK(list->allocator->deallocate(list->allocator, node,
                sizeof(ZyanListNode) + list->element_size, 1));
        }

        node = next;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Duplication                                                                                    */
/* ---------------------------------------------------------------------------------------------- */



/* ---------------------------------------------------------------------------------------------- */
/* Item access                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanListGetHeadNode(const ZyanList* list, const ZyanListNode** node)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *node = list->head;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListGetTailNode(const ZyanList* list, const ZyanListNode** node)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *node = list->tail;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListGetPrevNode(const ZyanListNode** node)
{
    if (!node || !*node)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *node = (*node)->prev;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListGetNextNode(const ZyanListNode** node)
{
    if (!node || !*node)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *node = (*node)->next;

    return ZYAN_STATUS_SUCCESS;
}

const void* ZyanListGetNodeData(const ZyanListNode* node)
{
    if (!node)
    {
        return ZYAN_NULL;
    }

    return (const void*)ZYCORE_LIST_GET_NODE_DATA(node);
}

ZyanStatus ZyanListGetNodeDataEx(const ZyanListNode* node, const void** value)
{
    if (!node)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *value = (const void*)ZYCORE_LIST_GET_NODE_DATA(node);

    return ZYAN_STATUS_SUCCESS;
}

void* ZyanListGetNodeDataMutable(const ZyanListNode* node)
{
    if (!node)
    {
        return ZYAN_NULL;
    }

    return ZYCORE_LIST_GET_NODE_DATA(node);
}

ZyanStatus ZyanListGetNodeDataMutableEx(const ZyanListNode* node, void** value)
{
    if (!node)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *value = ZYCORE_LIST_GET_NODE_DATA(node);

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListSetNodeData(const ZyanList* list, const ZyanListNode* node, const void* value)
{
    if (!list || !node || !value)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    if (list->destructor)
    {
        list->destructor(ZYCORE_LIST_GET_NODE_DATA(node));
    }

    ZYAN_ASSERT(list->element_size);
    ZYAN_MEMCPY(ZYCORE_LIST_GET_NODE_DATA(node), value, list->element_size);

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Insertion                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanListPushBack(ZyanList* list, const void* item)
{
    if (!list || !item)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanListNode* node;
    ZYAN_CHECK(ZyanListAllocateNode(list, &node));
    node->prev = list->tail;
    node->next = ZYAN_NULL;

    ZYAN_MEMCPY(ZYCORE_LIST_GET_NODE_DATA(node), item, list->element_size);

    if (!list->head)
    {
        list->head = node;
        list->tail = node;
    } else
    {
        list->tail->next = node;
        list->tail = node;
    }
    ++list->size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListPushFront(ZyanList* list, const void* item)
{
    if (!list || !item)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanListNode* node;
    ZYAN_CHECK(ZyanListAllocateNode(list, &node));
    node->prev = ZYAN_NULL;
    node->next = list->head;

    ZYAN_MEMCPY(ZYCORE_LIST_GET_NODE_DATA(node), item, list->element_size);

    if (!list->head)
    {
        list->head = node;
        list->tail = node;
    } else
    {
        list->head->prev= node;
        list->head = node;
    }
    ++list->size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListEmplaceBack(ZyanList* list, void** item, ZyanMemberFunction constructor)
{
    if (!list || !item)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanListNode* node;
    ZYAN_CHECK(ZyanListAllocateNode(list, &node));
    node->prev = list->tail;
    node->next = ZYAN_NULL;

    *item = ZYCORE_LIST_GET_NODE_DATA(node);
    if (constructor)
    {
        constructor(*item);
    }

    if (!list->head)
    {
        list->head = node;
        list->tail = node;
    } else
    {
        list->tail->next = node;
        list->tail = node;
    }
    ++list->size;

    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListEmplaceFront(ZyanList* list, void** item, ZyanMemberFunction constructor)
{
    if (!list || !item)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    ZyanListNode* node;
    ZYAN_CHECK(ZyanListAllocateNode(list, &node));
    node->prev = ZYAN_NULL;
    node->next = list->head;

    *item = ZYCORE_LIST_GET_NODE_DATA(node);
    if (constructor)
    {
        constructor(*item);
    }

    if (!list->head)
    {
        list->head = node;
        list->tail = node;
    } else
    {
        list->head->prev= node;
        list->head = node;
    }
    ++list->size;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Deletion                                                                                       */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanListPopBack(ZyanList* list)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (!list->tail)
    {
        return ZYAN_STATUS_INVALID_OPERATION;
    }

    ZyanListNode* const node = list->tail;

    if (list->destructor)
    {
        list->destructor(ZYCORE_LIST_GET_NODE_DATA(node));
    }

    list->tail = node->prev;
    if (list->tail)
    {
        list->tail->next = ZYAN_NULL;
    }
    if (list->head == node)
    {
        list->head = list->tail;
    }
    --list->size;

    return ZyanListDeallocateNode(list, node);
}

ZyanStatus ZyanListPopFront(ZyanList* list)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (!list->head)
    {
        return ZYAN_STATUS_INVALID_OPERATION;
    }

    ZyanListNode* const node = list->head;

    if (list->destructor)
    {
        list->destructor(ZYCORE_LIST_GET_NODE_DATA(node));
    }

    list->head = node->next;
    if (list->head)
    {
        list->head->prev = ZYAN_NULL;
    }
    if (list->tail == node)
    {
        list->tail = list->head;
    }
    --list->size;

    return ZyanListDeallocateNode(list, node);
}

ZyanStatus ZyanListRemove(ZyanList* list, const ZyanListNode* node)
{
    ZYAN_UNUSED(list);
    ZYAN_UNUSED(node);
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListRemoveRange(ZyanList* list, const ZyanListNode* first, const ZyanListNode* last)
{
    ZYAN_UNUSED(list);
    ZYAN_UNUSED(first);
    ZYAN_UNUSED(last);
    return ZYAN_STATUS_SUCCESS;
}

ZyanStatus ZyanListClear(ZyanList* list)
{
    return ZyanListResizeEx(list, 0, ZYAN_NULL);
}

/* ---------------------------------------------------------------------------------------------- */
/* Searching                                                                                      */
/* ---------------------------------------------------------------------------------------------- */



/* ---------------------------------------------------------------------------------------------- */
/* Memory management                                                                              */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanListResize(ZyanList* list, ZyanUSize size)
{
    return ZyanListResizeEx(list, size, ZYAN_NULL);
}

ZyanStatus ZyanListResizeEx(ZyanList* list, ZyanUSize size, const void* initializer)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }
    if (size == list->size)
    {
        return ZYAN_STATUS_SUCCESS;
    }

    if (size == 0)
    {
        const ZyanBool is_dynamic = (list->allocator != ZYAN_NULL);
        ZyanListNode* node = (is_dynamic || list->destructor) ? list->head : ZYAN_NULL;
        while (node)
        {
            if (list->destructor)
            {
                list->destructor(ZYCORE_LIST_GET_NODE_DATA(node));
            }

            ZyanListNode* const next = node->next;

            if (is_dynamic)
            {
                ZYAN_CHECK(list->allocator->deallocate(list->allocator, node,
                    sizeof(ZyanListNode) + list->element_size, 1));
            }

            node = next;
        }

        list->size = 0;
        list->head = 0;
        list->tail = 0;
        list->first_unused = ZYAN_NULL;

        return ZYAN_STATUS_SUCCESS;
    }

    if (size > list->size)
    {
        ZyanListNode* node;
        for (ZyanUSize i = list->size; i < size; ++i)
        {
            ZYAN_CHECK(ZyanListAllocateNode(list, &node));
            node->prev = list->tail;
            node->next = ZYAN_NULL;

            if (initializer)
            {
                ZYAN_MEMCPY(ZYCORE_LIST_GET_NODE_DATA(node), initializer, list->element_size);
            }

            if (!list->head)
            {
                list->head = node;
                list->tail = node;
            } else
            {
                list->tail->next = node;
                list->tail = node;
            }

            // `ZyanListAllocateNode` needs the list size
            ++list->size;
        }
    } else
    {
        for (ZyanUSize i = size; i < list->size; ++i)
        {
            ZyanListNode* const node = list->tail;

            if (list->destructor)
            {
                list->destructor(ZYCORE_LIST_GET_NODE_DATA(node));
            }

            list->tail = node->prev;
            if (list->tail)
            {
                list->tail->next = ZYAN_NULL;
            }

            ZYAN_CHECK(ZyanListDeallocateNode(list, node));
        }

        list->size = size;
    }

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */
/* Information                                                                                    */
/* ---------------------------------------------------------------------------------------------- */

ZyanStatus ZyanListGetSize(const ZyanList* list, ZyanUSize* size)
{
    if (!list)
    {
        return ZYAN_STATUS_INVALID_ARGUMENT;
    }

    *size = list->size;

    return ZYAN_STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- */

/* ============================================================================================== */
