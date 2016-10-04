/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <cstddef>
#include <set>

#include "mongo/bson/bsonobj.h"

namespace mongo {
namespace dotted_path_support {

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
BSONElement extractElementAtPath(const BSONObj& obj, StringData path);

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
BSONElement extractElementAtPathOrArrayAlongPath(const BSONObj& obj, const char*& path);

/**
 * Expands arrays along the specified path and adds all elements to the 'elements' set.
 *
 * The 'path' can be specified using a dotted notation in order to traverse through embedded objects
 * and array elements.
 *
 * This function fills 'arrayComponents' with the positions (starting at 0) of 'path' corresponding
 * to array values with multiple elements.
 *
 * Some examples:
 *
 *   Consider the document {a: [{b: 1}, {b: 2}]} and the path "a.b". The elements {b: 1} and {b: 2}
 *   would be added to the set. 'arrayComponents' would be set as std::set<size_t>{0U}.
 *
 *   Consider the document {a: [{b: [1, 2]}, {b: [2, 3]}]} and the path "a.b". The elements {b: 1},
 *   {b: 2}, and {b: 3} would be added to the set and 'arrayComponents' would be set as
 *   std::set<size_t>{0U, 1U} if 'expandArrayOnTrailingField' is true. The elements {b: [1, 2]} and
 *   {b: [2, 3]} would be added to the set and 'arrayComponents' would be set as
 *   std::set<size_t>{0U} if 'expandArrayOnTrailingField' is false.
 */
void extractAllElementsAlongPath(const BSONObj& obj,
                                 StringData path,
                                 BSONElementSet& elements,
                                 bool expandArrayOnTrailingField = true,
                                 std::set<std::size_t>* arrayComponents = nullptr);

void extractAllElementsAlongPath(const BSONObj& obj,
                                 StringData path,
                                 BSONElementMSet& elements,
                                 bool expandArrayOnTrailingField = true,
                                 std::set<std::size_t>* arrayComponents = nullptr);

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

}  // namespace dotted_path_support
}  // namespace mongo
