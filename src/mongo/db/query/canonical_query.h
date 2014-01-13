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
        static Status canonicalize(const QueryMessage& qm, CanonicalQuery** out);

        /**
         * For testing or for internal clients to use.
         */
        static Status canonicalize(const string& ns, const BSONObj& query, CanonicalQuery** out);

        static Status canonicalize(const string& ns, const BSONObj& query, long long skip,
                                   long long limit, CanonicalQuery** out);

        static Status canonicalize(const string& ns, const BSONObj& query, const BSONObj& sort,
                                   const BSONObj& proj, CanonicalQuery** out);

        static Status canonicalize(const string& ns, const BSONObj& query, const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip, long long limit,
                                   CanonicalQuery** out);

        static Status canonicalize(const string& ns, const BSONObj& query, const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip, long long limit,
                                   const BSONObj& hint,
                                   CanonicalQuery** out);

        static Status canonicalize(const string& ns, const BSONObj& query, const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip, long long limit,
                                   const BSONObj& hint,
                                   const BSONObj& minObj, const BSONObj& maxObj,
                                   bool snapshot, CanonicalQuery** out);

        // What namespace is this query over?
        const string& ns() const { return _pq->ns(); }

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
        PlanCacheKey getPlanCacheKey() const;

        // Debugging
        string toString() const;

        // TODO: Move this to query_validator.cpp
        static Status isValid(MatchExpression* root);

        /**
         * Returns the normalized version of the subtree rooted at 'root'.
         */
        static MatchExpression* normalizeTree(MatchExpression* root);

        /**
         * Traverses expression tree post-order.
         * Sorts children at each non-leaf node by (MatchType, path(), cacheKey)
         */
        static void sortTree(MatchExpression* tree);

    private:
        // You must go through canonicalize to create a CanonicalQuery.
        CanonicalQuery() { }

        /**
         * Normalize this canonical query. This should be done only when a
         * canonical query is constructed via canonicalize().
         *
         * Takes ownership of 'root'.
         */
        Status normalize(MatchExpression* root);

        /**
         * Computes and stores the cache key / query shape
         * for this query.
         */
        void generateCacheKey(void);

        // Takes ownership of lpq
        Status init(LiteParsedQuery* lpq);

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
