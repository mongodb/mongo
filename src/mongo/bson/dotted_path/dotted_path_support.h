// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonelement_comparator_interface.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string_view>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
namespace bson {

/**
 * Returns the element at the specified path. This function returns BSONElement() if the element
 * wasn't found.
 *
 * The 'path' can be specified using a dotted notation in order to traverse through embedded objects
 * and array elements.
 *
 * Some examples:
 *
 *   Consider the document {a: {b: 1}} and the path "a.b". An element with key="b" and value=1 would
 *   be returned.
 *
 *   Consider the document {a: [{b: 1}]} and the path "a.b". BSONElement() would be returned because
 *   the array value is actually an object with key="0" and value={b: 1}.
 *
 *   Consider the document {a: [{b: 1}]} and the path "a.0.b". An element with key="b" and value=1
 *   would be returned.
 */
BSONElement extractElementAtDottedPath(const BSONObj& obj, std::string_view path);

/**
 * Returns the element at the specified path, or the first element with an array value encountered
 * along the specified path. This function returns BSONElement() if the element wasn't found.
 *
 * The 'path' can be specified using a dotted notation in order to traverse through embedded objects
 * and array elements.
 *
 * This function modifies 'path' to be the suffix of the path that follows the first element with an
 * array value. If no such element is present, then 'path' is set as the empty string.
 *
 * Some examples:
 *
 *   Consider the document {a: {b: [1]}} and the path "a.b". An element with key="b" and value=1
 *   would be returned. 'path' would be changed to the empty string.
 *
 *   Consider the document {a: [{b: 1}]} and the path "a.b". An element with key="a" and
 *   value=[{b: 1}] would be returned. 'path' would be changed to the string "b".
 */
BSONElement extractElementAtOrArrayAlongDottedPath(const BSONObj& obj, const char*& path);

/**
 * Returns an owned BSONObj with elements in the same order as they appear in the 'pattern' object
 * and values extracted from 'obj'.
 *
 * The keys of the elements in the 'pattern' object can be specified using a dotted notation in
 * order to traverse through embedded objects and array elements. The values of the elements in the
 * 'pattern' object are ignored.
 *
 * If 'useNullIfMissing' is true and the key in the 'pattern' object isn't present in 'obj', then a
 * null value is appended to the returned value instead.
 *
 * Some examples:
 *
 *   Consider the document {a: 1, b: 1} and the template {b: ""}. The object {b: 1} would be
 *   returned.
 *
 *   Consider the document {a: {b: 1}} and the template {"a.b": ""}. The object {"a.b": 1} would be
 *   returned.
 *
 *   Consider the document {b: 1} and the template {a: "", b: ""}. The object {a: null, b: 1} would
 *   be returned if 'useNullIfMissing' is true. The object {b: 1} would be returned if
 *   'useNullIfMissing' is false.
 */
BSONObj extractElementsBasedOnTemplate(const BSONObj& obj,
                                       const BSONObj& pattern,
                                       bool useNullIfMissing = false);

/**
 * Returns an owned BSONObj with elements in the same order as they appear in the 'pattern' object.
 * The value of each element is null.
 */
BSONObj extractNullForAllFieldsBasedOnTemplate(const BSONObj& pattern);

/**
 * Compares two objects according to order of elements in the 'sortKey' object. This function
 * returns -1 if 'firstObj' < 'secondObj' according to 'sortKey', 0 if 'firstObj' == 'secondObj'
 * according to 'sortKey', and 1 if 'firstObj' > 'secondObj' according to 'sortKey'.
 *
 * If 'assumeDottedPaths' is true, then extractElementAtPath() is used to get the element associated
 * with the key of an element in the 'sortKey' object. If 'assumeDottedPaths' is false, then
 * BSONObj::getField() is used to get the element associated with the key of an element in the
 * 'sortKey' object. BSONObj::getField() searches the object for the key verbatim and does no
 * special handling to traverse through embedded objects and array elements when a "." character is
 * specified.
 *
 * Unlike with BSONObj::woCompare(), the elements don't need to be in the same order between
 * 'firstObj' and 'secondObj'.
 */
int compareObjectsAccordingToSort(const BSONObj& firstObj,
                                  const BSONObj& secondObj,
                                  const BSONObj& sortKey,
                                  bool assumeDottedPaths = false);

}  // namespace bson
}  // namespace mongo
