/**
 *    Copyright (C) 2013 MongoDB Inc.
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

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

/**
 * Methods used by several parts of the planning process.
 */
class QueryPlannerCommon {
public:
    /**
     * Does the tree rooted at 'root' have a node with matchType 'type'?
     *
     * If 'out' is not NULL, sets 'out' to the first node of type 'type' encountered.
     */
    static bool hasNode(const MatchExpression* root,
                        MatchExpression::MatchType type,
                        const MatchExpression** out = NULL) {
        if (type == root->matchType()) {
            if (NULL != out) {
                *out = root;
            }
            return true;
        }

        for (size_t i = 0; i < root->numChildren(); ++i) {
            if (hasNode(root->getChild(i), type, out)) {
                return true;
            }
        }
        return false;
    }

    /**
     * Assumes the provided BSONObj is of the form {field1: -+1, ..., field2: -+1}
     * Returns a BSONObj with the values negated.
     */
    static BSONObj reverseSortObj(const BSONObj& sortObj) {
        BSONObjBuilder reverseBob;
        BSONObjIterator it(sortObj);
        while (it.more()) {
            BSONElement elt = it.next();
            reverseBob.append(elt.fieldName(), elt.numberInt() * -1);
        }
        return reverseBob.obj();
    }

    /**
     * Traverses the tree rooted at 'node'.  For every STAGE_IXSCAN encountered, reverse
     * the scan direction and index bounds.
     */
    static void reverseScans(QuerySolutionNode* node);
};

}  // namespace mongo
