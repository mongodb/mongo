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
     * QueryPlanner's job is to provide an entry point to the query planning and optimization
     * process.
     */
    class QueryPlanner {
    public:
        enum Options {
            // You probably want to set this.
            DEFAULT = 0,

            // Set this if you don't want a table scan.
            // See http://docs.mongodb.org/manual/reference/parameters/
            NO_TABLE_SCAN = 1,
        };

        /**
         * Outputs a series of possible solutions for the provided 'query' into 'out'.  Uses the
         * provided indices to generate a solution.
         *
         * Caller owns pointers in *out.
         */
        static void plan(const CanonicalQuery& query,
                         const vector<IndexEntry>& indices,
                         size_t options,
                         vector<QuerySolution*>* out);
    private:

        //
        // Index Selection methods.
        //

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
        static bool compatible(const BSONElement& elt, const IndexEntry& index, MatchExpression* node);

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
        static void rateIndices(MatchExpression* node, string prefix,
                                const vector<IndexEntry>& indices);

        //
        // Collection Scan Data Access method.
        //

        /**
         * Return a CollectionScanNode that scans as requested in 'query'.
         */
        static QuerySolution* makeCollectionScan(const CanonicalQuery& query, bool tailable);

        //
        // Indexed Data Access methods.
        //
        // The inArrayOperator flag deserves some attention.  It is set when we're processing a child of
        // a MatchExpression::ALL or MatchExpression::ELEM_MATCH_OBJECT.
        //
        // When true, the following behavior changes for all methods below that take it as an argument:
        // 0. No deletion of MatchExpression(s).  In fact,
        // 1. No mutation of the MatchExpression at all.  We need the tree as-is in order to perform
        //    a filter on the entire tree.
        // 2. No fetches performed.  There will be a final fetch by the caller of buildIndexedDataAccess
        //    who set the value of inArrayOperator to true.
        // 3. No compound indices are used and no bounds are combined.  These are incorrect in the context
        //    of these operators.
        //

        /**
         * If 'inArrayOperator' is false, takes ownership of 'root'.
         */
        static QuerySolutionNode* buildIndexedDataAccess(MatchExpression* root,
                                                         bool inArrayOperator,
                                                         const vector<IndexEntry>& indices);

        /**
         * Takes ownership of 'root'.
         */
        static QuerySolutionNode* buildIndexedAnd(MatchExpression* root,
                                                  bool inArrayOperator,
                                                  const vector<IndexEntry>& indices);

        /**
         * Takes ownership of 'root'.
         */
        static QuerySolutionNode* buildIndexedOr(MatchExpression* root,
                                                 bool inArrayOperator,
                                                 const vector<IndexEntry>& indices);

        /**
         * Helper used by buildIndexedAnd and buildIndexedOr.
         *
         * The children of AND and OR nodes are sorted by the index that the subtree rooted at
         * that node uses.  Child nodes that use the same index are adjacent to one another to
         * facilitate grouping of index scans.  As such, the processing for AND and OR is
         * almost identical.
         *
         * See tagForSort and sortUsingTags in index_tag.h for details on ordering the children
         * of OR and AND.
         *
         * Does not take ownership of 'root' but may remove children from it.
         */
        static bool processIndexScans(MatchExpression* root,
                                      bool inArrayOperator,
                                      const vector<IndexEntry>& indices,
                                      vector<QuerySolutionNode*>* out);

        //
        // Helpers for creating an index scan.
        //

        /**
         * Create a new IndexScanNode.  The bounds for 'expr' are computed and placed into the
         * first field's OIL position.  The rest of the OILs are allocated but uninitialized.
         */
        static IndexScanNode* makeIndexScan(const BSONObj& indexKeyPattern, MatchExpression* expr,
                                            bool* exact);
        /**
         * Fill in any bounds that are missing in 'scan' with the "all values for this field"
         * interval.
         */
        static void finishIndexScan(IndexScanNode* scan, const BSONObj& indexKeyPattern);

        //
        // Analysis of Data Access
        //

        /**
         * In brief: performs sort and covering analysis.
         *
         * The solution rooted at 'solnRoot' provides data for the query, whether through some
         * configuration of indices or through a collection scan.  Additional stages may be required
         * to perform sorting, projection, or other operations that are independent of the source
         * of the data.  These stages are added atop 'solnRoot'.
         *
         * 'taggedRoot' is a copy of the parse tree.  Nodes in 'solnRoot' may point into it.
         *
         * Takes ownership of 'solnRoot' and 'taggedRoot'.
         *
         * Caller owns the returned QuerySolution.
         */
        static QuerySolution* analyzeDataAccess(const CanonicalQuery& query,
                                                QuerySolutionNode* solnRoot);
    };

}  // namespace mongo
