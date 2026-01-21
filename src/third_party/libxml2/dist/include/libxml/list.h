/**
 * @file
 * 
 * @brief lists interfaces
 * 
 * this module implement the list support used in
 * various place in the library.
 *
 * @copyright See Copyright for the status of this software.
 *
 * @author Gary Pennington
 */

#ifndef __XML_LINK_INCLUDE__
#define __XML_LINK_INCLUDE__

#include <libxml/xmlversion.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Linked list item
 *
 * @deprecated Don't use in new code.
 */
typedef struct _xmlLink xmlLink;
typedef xmlLink *xmlLinkPtr;

/**
 * Linked list
 *
 * @deprecated Don't use in new code.
 */
typedef struct _xmlList xmlList;
typedef xmlList *xmlListPtr;

/**
 * Callback function used to free data from a list.
 *
 * @param lk  the data to deallocate
 */
typedef void (*xmlListDeallocator) (xmlLink *lk);
/**
 * Callback function used to compare 2 data.
 *
 * @param data0  the first data
 * @param data1  the second data
 * @returns 0 is equality, -1 or 1 otherwise depending on the ordering.
 */
typedef int  (*xmlListDataCompare) (const void *data0, const void *data1);
/**
 * Callback function used when walking a list with #xmlListWalk.
 *
 * @param data  the data found in the list
 * @param user  extra user provided data to the walker
 * @returns 0 to stop walking the list, 1 otherwise.
 */
typedef int (*xmlListWalker) (const void *data, void *user);

/* Creation/Deletion */
XMLPUBFUN xmlList *
		xmlListCreate		(xmlListDeallocator deallocator,
	                                 xmlListDataCompare compare);
XMLPUBFUN void
		xmlListDelete		(xmlList *l);

/* Basic Operators */
XMLPUBFUN void *
		xmlListSearch		(xmlList *l,
					 void *data);
XMLPUBFUN void *
		xmlListReverseSearch	(xmlList *l,
					 void *data);
XMLPUBFUN int
		xmlListInsert		(xmlList *l,
					 void *data) ;
XMLPUBFUN int
		xmlListAppend		(xmlList *l,
					 void *data) ;
XMLPUBFUN int
		xmlListRemoveFirst	(xmlList *l,
					 void *data);
XMLPUBFUN int
		xmlListRemoveLast	(xmlList *l,
					 void *data);
XMLPUBFUN int
		xmlListRemoveAll	(xmlList *l,
					 void *data);
XMLPUBFUN void
		xmlListClear		(xmlList *l);
XMLPUBFUN int
		xmlListEmpty		(xmlList *l);
XMLPUBFUN xmlLink *
		xmlListFront		(xmlList *l);
XMLPUBFUN xmlLink *
		xmlListEnd		(xmlList *l);
XMLPUBFUN int
		xmlListSize		(xmlList *l);

XMLPUBFUN void
		xmlListPopFront		(xmlList *l);
XMLPUBFUN void
		xmlListPopBack		(xmlList *l);
XMLPUBFUN int
		xmlListPushFront	(xmlList *l,
					 void *data);
XMLPUBFUN int
		xmlListPushBack		(xmlList *l,
					 void *data);

/* Advanced Operators */
XMLPUBFUN void
		xmlListReverse		(xmlList *l);
XMLPUBFUN void
		xmlListSort		(xmlList *l);
XMLPUBFUN void
		xmlListWalk		(xmlList *l,
					 xmlListWalker walker,
					 void *user);
XMLPUBFUN void
		xmlListReverseWalk	(xmlList *l,
					 xmlListWalker walker,
					 void *user);
XMLPUBFUN void
		xmlListMerge		(xmlList *l1,
					 xmlList *l2);
XMLPUBFUN xmlList *
		xmlListDup		(xmlList *old);
XMLPUBFUN int
		xmlListCopy		(xmlList *cur,
					 xmlList *old);
/* Link operators */
XMLPUBFUN void *
		xmlLinkGetData          (xmlLink *lk);

/* xmlListUnique() */
/* xmlListSwap */

#ifdef __cplusplus
}
#endif

#endif /* __XML_LINK_INCLUDE__ */
