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

#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    /**
     * Methods for determining what fields and predicates can use indices.
     */
    class QueryPlannerIXSelect {
    public:
        /**
         * Return all the fields in the tree rooted at 'node' that we can use an index on
         * in order to answer the query.
         *
         * The 'prefix' argument is a path prefix to be prepended to any fields mentioned in
         * predicates encountered.  Some array operators specify a path prefix.
         */
        static void getFields(MatchExpression* node, string prefix, unordered_set<string>* out);

        /**
         * Find all indices prefixed by fields we have predicates over.  Only these indices are
         * useful in answering the query.
         */
        static void findRelevantIndices(const unordered_set<string>& fields,
                                        const vector<IndexEntry>& indices,
                                        vector<IndexEntry>* out);

        /**
         * Return true if the index key pattern field 'elt' (which belongs to 'index') can be used
         * to answer the predicate 'node'.
         *
         * For example, {field: "hashed"} can only be used with sets of equalities.
         *              {field: "2d"} can only be used with some geo predicates.
         *              {field: "2dsphere"} can only be used with some other geo predicates.
         */
        static bool compatible(const BSONElement& elt,
                               const IndexEntry& index,
                               MatchExpression* node);

        /**
         * Determine how useful all of our relevant 'indices' are to all predicates in the subtree
         * rooted at 'node'.  Affixes a RelevantTag to all predicate nodes which can use an index.
         *
         * 'prefix' is a path prefix that should be prepended to any path (certain array operators
         * imply a path prefix).
         *
         * For an index to be useful to a predicate, the index must be compatible (see above).
         *
         * If an index is prefixed by the predicate's path, it's always useful.
         *
         * If an index is compound but not prefixed by a predicate's path, it's only useful if
         * there exists another predicate that 1. will use that index and 2. is related to the
         * original predicate by having an AND as a parent.
         */
        static void rateIndices(MatchExpression* node,
                                string prefix,
                                const vector<IndexEntry>& indices);
    };

}  // namespace mongo
