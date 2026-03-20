/**
*******************************************************************************
* @file json_object_iterator.c
*
* Copyright (c) 2009-2012 Hewlett-Packard Development Company, L.P.
*
* This library is free software; you can redistribute it and/or modify
* it under the terms of the MIT license. See COPYING for details.
*
*******************************************************************************
*/
#include "config.h"

#include <stddef.h>

#include "json.h"
#include "json_object_private.h"

#include "json_object_iterator.h"

/**
 * How It Works
 *
 * For each JSON Object, json-c maintains a linked list of zero
 * or more lh_entry (link-hash entry) structures inside the
 * Object's link-hash table (lh_table).
 *
 * Each lh_entry structure on the JSON Object's linked list
 * represents a single name/value pair.  The "next" field of the
 * last lh_entry in the list is set to NULL, which terminates
 * the list.
 *
 * We represent a valid iterator that refers to an actual
 * name/value pair via a pointer to the pair's lh_entry
 * structure set as the iterator's opaque_ field.
 *
 * We follow json-c's current pair list representation by
 * representing a valid "end" iterator (one that refers past the
 * last pair) with a NULL value in the iterator's opaque_ field.
 *
 * A JSON Object without any pairs in it will have the "head"
 * field of its lh_table structure set to NULL.  For such an
 * object, json_object_iter_begin will return an iterator with
 * the opaque_ field set to NULL, which is equivalent to the
 * "end" iterator.
 *
 * When iterating, we simply update the iterator's opaque_ field
 * to point to the next lh_entry structure in the linked list.
 * opaque_ will become NULL once we iterate past the last pair
 * in the list, which makes the iterator equivalent to the "end"
 * iterator.
 */

/// Our current representation of the "end" iterator;
///
/// @note May not always be NULL
static const void *kObjectEndIterValue = NULL;

/**
 * ****************************************************************************
 */
struct json_object_iterator json_object_iter_begin(struct json_object *obj)
{
	struct json_object_iterator iter;
	struct lh_table *pTable;

	/// @note json_object_get_object will return NULL if passed NULL
	///       or a non-json_type_object instance
	pTable = json_object_get_object(obj);
	JASSERT(NULL != pTable);

	/// @note For a pair-less Object, head is NULL, which matches our
	///       definition of the "end" iterator
	iter.opaque_ = lh_table_head(pTable);
	return iter;
}

/**
 * ****************************************************************************
 */
struct json_object_iterator json_object_iter_end(const struct json_object *obj)
{
	struct json_object_iterator iter;

	JASSERT(NULL != obj);
	JASSERT(json_object_is_type(obj, json_type_object));

	iter.opaque_ = kObjectEndIterValue;

	return iter;
}

/**
 * ****************************************************************************
 */
void json_object_iter_next(struct json_object_iterator *iter)
{
	JASSERT(NULL != iter);
	JASSERT(kObjectEndIterValue != iter->opaque_);

	iter->opaque_ = lh_entry_next(((const struct lh_entry *)iter->opaque_));
}

/**
 * ****************************************************************************
 */
const char *json_object_iter_peek_name(const struct json_object_iterator *iter)
{
	JASSERT(NULL != iter);
	JASSERT(kObjectEndIterValue != iter->opaque_);

	return (const char *)(((const struct lh_entry *)iter->opaque_)->k);
}

/**
 * ****************************************************************************
 */
struct json_object *json_object_iter_peek_value(const struct json_object_iterator *iter)
{
	JASSERT(NULL != iter);
	JASSERT(kObjectEndIterValue != iter->opaque_);

	return (struct json_object *)lh_entry_v((const struct lh_entry *)iter->opaque_);
}

/**
 * ****************************************************************************
 */
json_bool json_object_iter_equal(const struct json_object_iterator *iter1,
                                 const struct json_object_iterator *iter2)
{
	JASSERT(NULL != iter1);
	JASSERT(NULL != iter2);

	return (iter1->opaque_ == iter2->opaque_);
}

/**
 * ****************************************************************************
 */
struct json_object_iterator json_object_iter_init_default(void)
{
	struct json_object_iterator iter;

	/**
	 * @note Make this a negative, invalid value, such that
	 *       accidental access to it would likely be trapped by the
	 *       hardware as an invalid address.
	 */
	iter.opaque_ = NULL;

	return iter;
}
