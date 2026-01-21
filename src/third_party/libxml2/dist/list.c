/*
 * list.c: lists handling implementation
 *
 * Copyright (C) 2000 Gary Pennington and Daniel Veillard.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. THE AUTHORS AND
 * CONTRIBUTORS ACCEPT NO RESPONSIBILITY IN ANY CONCEIVABLE MANNER.
 *
 * Author: Gary Pennington
 */

#define IN_LIBXML
#include "libxml.h"

#include <stdlib.h>
#include <string.h>
#include <libxml/xmlmemory.h>
#include <libxml/xmlerror.h>
#include <libxml/list.h>

/*
 * Type definition are kept internal
 */

struct _xmlLink
{
    struct _xmlLink *next;
    struct _xmlLink *prev;
    void *data;
};

struct _xmlList
{
    xmlLinkPtr sentinel;
    void (*linkDeallocator)(xmlLinkPtr );
    int (*linkCompare)(const void *, const void*);
};

/************************************************************************
 *                                    *
 *                Interfaces                *
 *                                    *
 ************************************************************************/

/**
 * Unlink and deallocate `lk` from list `l`
 *
 * @param l  a list
 * @param lk  a link
 */
static void
xmlLinkDeallocator(xmlListPtr l, xmlLinkPtr lk)
{
    (lk->prev)->next = lk->next;
    (lk->next)->prev = lk->prev;
    if(l->linkDeallocator)
        l->linkDeallocator(lk);
    xmlFree(lk);
}

/**
 * Compares two arbitrary data
 *
 * @param data0  first data
 * @param data1  second data
 * @returns -1, 0 or 1 depending on whether data1 is greater equal or smaller
 *          than data0
 */
static int
xmlLinkCompare(const void *data0, const void *data1)
{
    if (data0 < data1)
        return (-1);
    else if (data0 == data1)
	return (0);
    return (1);
}

/**
 * Search data in the ordered list walking from the beginning
 *
 * @param l  a list
 * @param data  a data
 * @returns the link containing the data or NULL
 */
static xmlLinkPtr
xmlListLowerSearch(xmlListPtr l, void *data)
{
    xmlLinkPtr lk;

    if (l == NULL)
        return(NULL);
    for(lk = l->sentinel->next;lk != l->sentinel && l->linkCompare(lk->data, data) <0 ;lk = lk->next);
    return lk;
}

/**
 * Search data in the ordered list walking backward from the end
 *
 * @param l  a list
 * @param data  a data
 * @returns the link containing the data or NULL
 */
static xmlLinkPtr
xmlListHigherSearch(xmlListPtr l, void *data)
{
    xmlLinkPtr lk;

    if (l == NULL)
        return(NULL);
    for(lk = l->sentinel->prev;lk != l->sentinel && l->linkCompare(lk->data, data) >0 ;lk = lk->prev);
    return lk;
}

/**
 * Search data in the list
 *
 * @param l  a list
 * @param data  a data
 * @returns the link containing the data or NULL
 */
static xmlLinkPtr
xmlListLinkSearch(xmlListPtr l, void *data)
{
    xmlLinkPtr lk;
    if (l == NULL)
        return(NULL);
    lk = xmlListLowerSearch(l, data);
    if (lk == l->sentinel)
        return NULL;
    else {
        if (l->linkCompare(lk->data, data) ==0)
            return lk;
        return NULL;
    }
}

/**
 * Search data in the list processing backward
 *
 * @param l  a list
 * @param data  a data
 * @returns the link containing the data or NULL
 */
static xmlLinkPtr
xmlListLinkReverseSearch(xmlListPtr l, void *data)
{
    xmlLinkPtr lk;
    if (l == NULL)
        return(NULL);
    lk = xmlListHigherSearch(l, data);
    if (lk == l->sentinel)
        return NULL;
    else {
        if (l->linkCompare(lk->data, data) ==0)
            return lk;
        return NULL;
    }
}

/**
 * Create a new list
 *
 * @param deallocator  an optional deallocator function
 * @param compare  an optional comparison function
 * @returns the new list or NULL in case of error
 */
xmlList *
xmlListCreate(xmlListDeallocator deallocator, xmlListDataCompare compare)
{
    xmlListPtr l;
    l = (xmlListPtr)xmlMalloc(sizeof(xmlList));
    if (l == NULL)
        return (NULL);
    /* Initialize the list to NULL */
    memset(l, 0, sizeof(xmlList));

    /* Add the sentinel */
    l->sentinel = (xmlLinkPtr)xmlMalloc(sizeof(xmlLink));
    if (l->sentinel == NULL) {
	xmlFree(l);
        return (NULL);
    }
    l->sentinel->next = l->sentinel;
    l->sentinel->prev = l->sentinel;
    l->sentinel->data = NULL;

    /* If there is a link deallocator, use it */
    if (deallocator != NULL)
        l->linkDeallocator = deallocator;
    /* If there is a link comparator, use it */
    if (compare != NULL)
        l->linkCompare = compare;
    else /* Use our own */
        l->linkCompare = xmlLinkCompare;
    return l;
}

/**
 * Search the list for an existing value of `data`
 *
 * @param l  a list
 * @param data  a search value
 * @returns the value associated to `data` or NULL in case of error
 */
void *
xmlListSearch(xmlList *l, void *data)
{
    xmlLinkPtr lk;
    if (l == NULL)
        return(NULL);
    lk = xmlListLinkSearch(l, data);
    if (lk)
        return (lk->data);
    return NULL;
}

/**
 * Search the list in reverse order for an existing value of `data`
 *
 * @param l  a list
 * @param data  a search value
 * @returns the value associated to `data` or NULL in case of error
 */
void *
xmlListReverseSearch(xmlList *l, void *data)
{
    xmlLinkPtr lk;
    if (l == NULL)
        return(NULL);
    lk = xmlListLinkReverseSearch(l, data);
    if (lk)
        return (lk->data);
    return NULL;
}

/**
 * Insert data in the ordered list at the beginning for this value
 *
 * @param l  a list
 * @param data  the data
 * @returns 0 in case of success, 1 in case of failure
 */
int
xmlListInsert(xmlList *l, void *data)
{
    xmlLinkPtr lkPlace, lkNew;

    if (l == NULL)
        return(1);
    lkPlace = xmlListLowerSearch(l, data);
    /* Add the new link */
    lkNew = (xmlLinkPtr) xmlMalloc(sizeof(xmlLink));
    if (lkNew == NULL)
        return (1);
    lkNew->data = data;
    lkPlace = lkPlace->prev;
    lkNew->next = lkPlace->next;
    (lkPlace->next)->prev = lkNew;
    lkPlace->next = lkNew;
    lkNew->prev = lkPlace;
    return 0;
}

/**
 * Insert data in the ordered list at the end for this value
 *
 * @param l  a list
 * @param data  the data
 * @returns 0 in case of success, 1 in case of failure
 */
int xmlListAppend(xmlList *l, void *data)
{
    xmlLinkPtr lkPlace, lkNew;

    if (l == NULL)
        return(1);
    lkPlace = xmlListHigherSearch(l, data);
    /* Add the new link */
    lkNew = (xmlLinkPtr) xmlMalloc(sizeof(xmlLink));
    if (lkNew == NULL)
        return (1);
    lkNew->data = data;
    lkNew->next = lkPlace->next;
    (lkPlace->next)->prev = lkNew;
    lkPlace->next = lkNew;
    lkNew->prev = lkPlace;
    return 0;
}

/**
 * Deletes the list and its associated data
 *
 * @param l  a list
 */
void xmlListDelete(xmlList *l)
{
    if (l == NULL)
        return;

    xmlListClear(l);
    xmlFree(l->sentinel);
    xmlFree(l);
}

/**
 * Remove the first instance associated to data in the list
 *
 * @param l  a list
 * @param data  list data
 * @returns 1 if a deallocation occurred, or 0 if not found
 */
int
xmlListRemoveFirst(xmlList *l, void *data)
{
    xmlLinkPtr lk;

    if (l == NULL)
        return(0);
    /*Find the first instance of this data */
    lk = xmlListLinkSearch(l, data);
    if (lk != NULL) {
        xmlLinkDeallocator(l, lk);
        return 1;
    }
    return 0;
}

/**
 * Remove the last instance associated to data in the list
 *
 * @param l  a list
 * @param data  list data
 * @returns 1 if a deallocation occurred, or 0 if not found
 */
int
xmlListRemoveLast(xmlList *l, void *data)
{
    xmlLinkPtr lk;

    if (l == NULL)
        return(0);
    /*Find the last instance of this data */
    lk = xmlListLinkReverseSearch(l, data);
    if (lk != NULL) {
	xmlLinkDeallocator(l, lk);
        return 1;
    }
    return 0;
}

/**
 * Remove the all instance associated to data in the list
 *
 * @param l  a list
 * @param data  list data
 * @returns the number of deallocation, or 0 if not found
 */
int
xmlListRemoveAll(xmlList *l, void *data)
{
    int count=0;

    if (l == NULL)
        return(0);

    while(xmlListRemoveFirst(l, data))
        count++;
    return count;
}

/**
 * Remove the all data in the list
 *
 * @param l  a list
 */
void
xmlListClear(xmlList *l)
{
    xmlLinkPtr  lk;

    if (l == NULL)
        return;
    lk = l->sentinel->next;
    while(lk != l->sentinel) {
        xmlLinkPtr next = lk->next;

        xmlLinkDeallocator(l, lk);
        lk = next;
    }
}

/**
 * Is the list empty ?
 *
 * @param l  a list
 * @returns 1 if the list is empty, 0 if not empty and -1 in case of error
 */
int
xmlListEmpty(xmlList *l)
{
    if (l == NULL)
        return(-1);
    return (l->sentinel->next == l->sentinel);
}

/**
 * Get the first element in the list
 *
 * @param l  a list
 * @returns the first element in the list, or NULL
 */
xmlLink *
xmlListFront(xmlList *l)
{
    if (l == NULL)
        return(NULL);
    return (l->sentinel->next);
}

/**
 * Get the last element in the list
 *
 * @param l  a list
 * @returns the last element in the list, or NULL
 */
xmlLink *
xmlListEnd(xmlList *l)
{
    if (l == NULL)
        return(NULL);
    return (l->sentinel->prev);
}

/**
 * Get the number of elements in the list
 *
 * @param l  a list
 * @returns the number of elements in the list or -1 in case of error
 */
int
xmlListSize(xmlList *l)
{
    xmlLinkPtr lk;
    int count=0;

    if (l == NULL)
        return(-1);
    /* TODO: keep a counter in xmlList instead */
    for(lk = l->sentinel->next; lk != l->sentinel; lk = lk->next, count++);
    return count;
}

/**
 * Removes the first element in the list
 *
 * @param l  a list
 */
void
xmlListPopFront(xmlList *l)
{
    if(!xmlListEmpty(l))
        xmlLinkDeallocator(l, l->sentinel->next);
}

/**
 * Removes the last element in the list
 *
 * @param l  a list
 */
void
xmlListPopBack(xmlList *l)
{
    if(!xmlListEmpty(l))
        xmlLinkDeallocator(l, l->sentinel->prev);
}

/**
 * add the new data at the beginning of the list
 *
 * @param l  a list
 * @param data  new data
 * @returns 1 if successful, 0 otherwise
 */
int
xmlListPushFront(xmlList *l, void *data)
{
    xmlLinkPtr lkPlace, lkNew;

    if (l == NULL)
        return(0);
    lkPlace = l->sentinel;
    /* Add the new link */
    lkNew = (xmlLinkPtr) xmlMalloc(sizeof(xmlLink));
    if (lkNew == NULL)
        return (0);
    lkNew->data = data;
    lkNew->next = lkPlace->next;
    (lkPlace->next)->prev = lkNew;
    lkPlace->next = lkNew;
    lkNew->prev = lkPlace;
    return 1;
}

/**
 * add the new data at the end of the list
 *
 * @param l  a list
 * @param data  new data
 * @returns 1 if successful, 0 otherwise
 */
int
xmlListPushBack(xmlList *l, void *data)
{
    xmlLinkPtr lkPlace, lkNew;

    if (l == NULL)
        return(0);
    lkPlace = l->sentinel->prev;
    /* Add the new link */
    lkNew = (xmlLinkPtr)xmlMalloc(sizeof(xmlLink));
    if (lkNew == NULL)
        return (0);
    lkNew->data = data;
    lkNew->next = lkPlace->next;
    (lkPlace->next)->prev = lkNew;
    lkPlace->next = lkNew;
    lkNew->prev = lkPlace;
    return 1;
}

/**
 * See Returns.
 *
 * @param lk  a link
 * @returns a pointer to the data referenced from this link
 */
void *
xmlLinkGetData(xmlLink *lk)
{
    if (lk == NULL)
        return(NULL);
    return lk->data;
}

/**
 * Reverse the order of the elements in the list
 *
 * @param l  a list
 */
void
xmlListReverse(xmlList *l)
{
    xmlLinkPtr lk;
    xmlLinkPtr lkPrev;

    if (l == NULL)
        return;
    lkPrev = l->sentinel;
    for (lk = l->sentinel->next; lk != l->sentinel; lk = lk->next) {
        lkPrev->next = lkPrev->prev;
        lkPrev->prev = lk;
        lkPrev = lk;
    }
    /* Fix up the last node */
    lkPrev->next = lkPrev->prev;
    lkPrev->prev = lk;
}

/**
 * Sort all the elements in the list
 *
 * @param l  a list
 */
void
xmlListSort(xmlList *l)
{
    xmlListPtr lTemp;

    if (l == NULL)
        return;
    if(xmlListEmpty(l))
        return;

    /* I think that the real answer is to implement quicksort, the
     * alternative is to implement some list copying procedure which
     * would be based on a list copy followed by a clear followed by
     * an insert. This is slow...
     */

    lTemp = xmlListDup(l);
    if (lTemp == NULL)
        return;
    xmlListClear(l);
    xmlListMerge(l, lTemp);
    xmlListDelete(lTemp);
}

/**
 * Walk all the element of the first from first to last and
 * apply the walker function to it
 *
 * @param l  a list
 * @param walker  a processing function
 * @param user  a user parameter passed to the walker function
 */
void
xmlListWalk(xmlList *l, xmlListWalker walker, void *user) {
    xmlLinkPtr lk;

    if ((l == NULL) || (walker == NULL))
        return;
    for(lk = l->sentinel->next; lk != l->sentinel; lk = lk->next) {
        if((walker(lk->data, user)) == 0)
                break;
    }
}

/**
 * Walk all the element of the list in reverse order and
 * apply the walker function to it
 *
 * @param l  a list
 * @param walker  a processing function
 * @param user  a user parameter passed to the walker function
 */
void
xmlListReverseWalk(xmlList *l, xmlListWalker walker, void *user) {
    xmlLinkPtr lk;

    if ((l == NULL) || (walker == NULL))
        return;
    for(lk = l->sentinel->prev; lk != l->sentinel; lk = lk->prev) {
        if((walker(lk->data, user)) == 0)
                break;
    }
}

/**
 * include all the elements of the second list in the first one and
 * clear the second list
 *
 * @param l1  the original list
 * @param l2  the new list
 */
void
xmlListMerge(xmlList *l1, xmlList *l2)
{
    xmlListCopy(l1, l2);
    xmlListClear(l2);
}

/**
 * Duplicate the list
 *
 * @param old  the list
 * @returns a new copy of the list or NULL in case of error
 */
xmlList *
xmlListDup(xmlList *old)
{
    xmlListPtr cur;

    if (old == NULL)
        return(NULL);
    /* Hmmm, how to best deal with allocation issues when copying
     * lists. If there is a de-allocator, should responsibility lie with
     * the new list or the old list. Surely not both. I'll arbitrarily
     * set it to be the old list for the time being whilst I work out
     * the answer
     */
    cur = xmlListCreate(NULL, old->linkCompare);
    if (cur == NULL)
        return (NULL);
    if (0 != xmlListCopy(cur, old))
        return NULL;
    return cur;
}

/**
 * Move all the element from the old list in the new list
 *
 * @param cur  the new list
 * @param old  the old list
 * @returns 0 in case of success 1 in case of error
 */
int
xmlListCopy(xmlList *cur, xmlList *old)
{
    /* Walk the old tree and insert the data into the new one */
    xmlLinkPtr lk;

    if ((old == NULL) || (cur == NULL))
        return(1);
    for(lk = old->sentinel->next; lk != old->sentinel; lk = lk->next) {
        if (0 !=xmlListInsert(cur, lk->data)) {
            xmlListDelete(cur);
            return (1);
        }
    }
    return (0);
}
/* xmlListUnique() */
/* xmlListSwap */
