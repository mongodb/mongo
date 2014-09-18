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

#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/lite_parsed_query.h"
#include "mongo/db/query/parsed_projection.h"

namespace mongo {

    // TODO: Is this binary data really?
    typedef std::string PlanCacheKey;

    class CanonicalQuery {
    public:
        /**
         * Caller owns the pointer in 'out' if any call to canonicalize returns Status::OK().
         *
         * Used for legacy find through the OP_QUERY message.
         */
        static Status canonicalize(const QueryMessage& qm,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        /**
         * Takes ownership of 'lpq'.
         *
         * Caller owns the pointer in 'out' if any call to canonicalize returns Status::OK().
         *
         * Used for finds using the find command path.
         */
        static Status canonicalize(LiteParsedQuery* lpq,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        /**
         * For testing or for internal clients to use.
         */

        /**
         * Used for creating sub-queries from an existing CanonicalQuery.
         *
         * 'root' must be an expression in baseQuery.root().
         *
         * Does not take ownership of 'root'.
         */
        static Status canonicalize(const CanonicalQuery& baseQuery,
                                   MatchExpression* root,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        static Status canonicalize(const std::string& ns,
                                   const BSONObj& query,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        static Status canonicalize(const std::string& ns, 
                                   const BSONObj& query,
                                   long long skip,
                                   long long limit, 
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        static Status canonicalize(const std::string& ns,
                                   const BSONObj& query,
                                   const BSONObj& sort,
                                   const BSONObj& proj, 
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        static Status canonicalize(const std::string& ns, 
                                   const BSONObj& query,
                                   const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip,
                                   long long limit,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        static Status canonicalize(const std::string& ns,
                                   const BSONObj& query,
                                   const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip,
                                   long long limit,
                                   const BSONObj& hint,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        static Status canonicalize(const std::string& ns,
                                   const BSONObj& query,
                                   const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip,
                                   long long limit,
                                   const BSONObj& hint,
                                   const BSONObj& minObj,
                                   const BSONObj& maxObj,
                                   bool snapshot,
                                   bool explain,
                                   CanonicalQuery** out,
                                   const MatchExpressionParser::WhereCallback& whereCallback =
                                            MatchExpressionParser::WhereCallback());

        /**
         * Returns true if "query" describes an exact-match query on _id, possibly with
         * the $isolated/$atomic modifier.
         */
        static bool isSimpleIdQuery(const BSONObj& query);

        // What namespace is this query over?
        const std::string& ns() const { return _pq->ns(); }

        //
        // Accessors for the query
        //
        MatchExpression* root() const { return _root.get(); }
        BSONObj getQueryObj() const { return _pq->getFilter(); }
        const LiteParsedQuery& getParsed() const { return *_pq; }
        const ParsedProjection* getProj() const { return _proj.get(); }

        /**
         * Get the cache key for this canonical query.
         */
        const PlanCacheKey& getPlanCacheKey() const;

        // Debugging
        std::string toString() const;
        std::string toStringShort() const;

        /**
         * Validates match expression, checking for certain
         * combinations of operators in match expression and
         * query options in LiteParsedQuery.
         * Since 'root' is derived from 'filter' in LiteParsedQuery,
         * 'filter' is not validated.
         *
         * TODO: Move this to query_validator.cpp
         */
        static Status isValid(MatchExpression* root, const LiteParsedQuery& parsed);

        /**
         * Returns the normalized version of the subtree rooted at 'root'.
         *
         * Takes ownership of 'root'.
         */
        static MatchExpression* normalizeTree(MatchExpression* root);

        /**
         * Traverses expression tree post-order.
         * Sorts children at each non-leaf node by (MatchType, path(), cacheKey)
         */
        static void sortTree(MatchExpression* tree);

        /**
         * Returns a count of 'type' nodes in expression tree.
         */
        static size_t countNodes(const MatchExpression* root, MatchExpression::MatchType type);

        /**
         * Takes ownership of 'tree'.  Performs some rewriting of the query to a logically
         * equivalent but more digestible form.
         *
         * TODO: This doesn't entirely belong here.  Really we'd do this while exploring
         * solutions in an enumeration setting but given the current lack of pruning
         * while exploring the enumeration space we do it here.
         */
        static MatchExpression* logicalRewrite(MatchExpression* tree);

    private:
        // You must go through canonicalize to create a CanonicalQuery.
        CanonicalQuery() { }

        /**
         * Computes and stores the cache key / query shape
         * for this query.
         */
        void generateCacheKey(void);

        /**
         * Takes ownership of 'root' and 'lpq'.
         */
        Status init(LiteParsedQuery* lpq,
                    const MatchExpressionParser::WhereCallback& whereCallback,
                    MatchExpression* root);

        scoped_ptr<LiteParsedQuery> _pq;

        // _root points into _pq->getFilter()
        scoped_ptr<MatchExpression> _root;

        scoped_ptr<ParsedProjection> _proj;

        /**
         * Cache key is a string-ified combination of the query and sort obfuscated
         * for minimal user comprehension.
         */
        PlanCacheKey _cacheKey;
    };

}  // namespace mongo
