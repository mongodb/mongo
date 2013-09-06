/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/mutable/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/platform/cstdint.h"

namespace mongo {

    namespace pathsupport {

        // Cap on the number of nulls we'll add to an array if we're inserting to an index that
        // doesn't exist.
        static const size_t kMaxPaddingAllowed = 1500000;

        /**
         * Finds the longest portion of 'prefix' that exists in document rooted at 'root' and is
         * "viable." A viable path is one that, if fully created on a given doc, would not
         * change the existing types of any fields in that doc. (See examples below.)
         *
         * If a prefix indeed exists, 'idxFound' is set to indicate how many parts in common
         * 'prefix' and 'doc' have. 'elemFound' would point to the Element corresponding to
         * prefix[idxFound] in 'doc'. The call would return an OK status in this case.
         *
         * If a prefix is not viable, returns a status "PathNotViable". 'idxFound' is set to
         * indicate the part in the document that caused the path to be not viable. 'elemFound'
         * would point to the Element corresponding to prefix[idxFound] in 'doc'.
         *
         * If a prefix does not exist, the call returns "NonExistentPath". 'elemFound' and
         * 'idxFound' are indeterminate in this case.
         *
         * Definition of a "Viable Path":
         *
         *   A field reference 'p_1.p_2.[...].p_n', where 'p_i' is a field part, is said to be
         *   a viable path in a given document D if the creation of each part 'p_i', 0 <= i < n
         *   in D does not force 'p_i' to change types. In other words, no existing 'p_i' in D
         *   may have a different type, other than the 'p_n'.
         *
         *   'a.b.c' is a viable path in {a: {b: {c: 1}}}
         *   'a.b.c' is a viable path in {a: {b: {c: {d: 1}}}}
         *   'a.b.c' is NOT a viable path in {a: {b: 1}}, because b would have changed types
         *   'a.0.b' is a viable path in {a: [{b: 1}, {c: 1}]}
         *   'a.0.b' is a viable path in {a: {"0": {b: 1}}}
         *   'a.0.b' is NOT a viable path in {a: 1}, because a would have changed types
         *   'a.5.b' is a viable path in in {a: []} (padding would occur)
         */
        Status findLongestPrefix(const FieldRef& prefix,
                                 mutablebson::Element root,
                                 size_t* idxFound,
                                 mutablebson::Element* elemFound);

        /**
         * Creates the parts 'prefix[idxRoot]', 'prefix[idxRoot+1]', ...,
         * 'prefix[<numParts>-1]' under 'elemFound' and adds 'newElem' as a child of that
         * path. Returns OK, if successful, or an error code describing why not, otherwise.
         *
         * createPathAt is designed to work with 'findLongestPrefix' in that it can create the
         * field parts in 'prefix' that are missing from a given document. 'elemFound' points
         * to the element in the doc that is the parent of prefix[idxRoot].
         */
        Status createPathAt(const FieldRef& prefix,
                            size_t idxRoot,
                            mutablebson::Element elemFound,
                            mutablebson::Element newElem);

    } // namespace pathsupport

} // namespace mongo
