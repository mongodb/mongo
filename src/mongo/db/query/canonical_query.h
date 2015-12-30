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

class CanonicalQuery {
public:
    /**
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * Used for legacy find through the OP_QUERY message.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        const QueryMessage& qm, const ExtensionsCallback& extensionsCallback);

    /**
     * Takes ownership of 'lpq'.
     *
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * Used for finds using the find command path.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(LiteParsedQuery* lpq,
                                                                    const ExtensionsCallback&);

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
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        const CanonicalQuery& baseQuery,
        MatchExpression* root,
        const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss, const BSONObj& query, const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss,
        const BSONObj& query,
        bool explain,
        const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss,
        const BSONObj& query,
        long long skip,
        long long limit,
        const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss,
        const BSONObj& query,
        const BSONObj& sort,
        const BSONObj& proj,
        const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss,
        const BSONObj& query,
        const BSONObj& sort,
        const BSONObj& proj,
        long long skip,
        long long limit,
        const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss,
        const BSONObj& query,
        const BSONObj& sort,
        const BSONObj& proj,
        long long skip,
        long long limit,
        const BSONObj& hint,
        const ExtensionsCallback& extensionsCallback);

    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        NamespaceString nss,
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
        const ExtensionsCallback& extensionsCallback);

    /**
     * Returns true if "query" describes an exact-match query on _id, possibly with
     * the $isolated/$atomic modifier.
     */
    static bool isSimpleIdQuery(const BSONObj& query);

    const NamespaceString& nss() const {
        return _pq->nss();
    }
    const std::string& ns() const {
        return _pq->nss().ns();
    }

    //
    // Accessors for the query
    //
    MatchExpression* root() const {
        return _root.get();
    }
    BSONObj getQueryObj() const {
        return _pq->getFilter();
    }
    const LiteParsedQuery& getParsed() const {
        return *_pq;
    }
    const ParsedProjection* getProj() const {
        return _proj.get();
    }

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
     * Sorts children at each non-leaf node by (MatchType, path(), children, number of children)
     */
    static void sortTree(MatchExpression* tree);

    /**
     * Returns a count of 'type' nodes in expression tree.
     */
    static size_t countNodes(const MatchExpression* root, MatchExpression::MatchType type);

    /**
     * Returns true if this canonical query converted extensions such as $where and $text into
     * no-ops during parsing.
     *
     * Queries with a no-op extension context are special because they can be parsed and planned,
     * but they cannot be executed.
     */
    bool hasNoopExtensions() const {
        return _hasNoopExtensions;
    }

private:
    // You must go through canonicalize to create a CanonicalQuery.
    CanonicalQuery() {}

    /**
     * Takes ownership of 'root' and 'lpq'.
     */
    Status init(LiteParsedQuery* lpq,
                const ExtensionsCallback& extensionsCallback,
                MatchExpression* root);

    std::unique_ptr<LiteParsedQuery> _pq;

    // _root points into _pq->getFilter()
    std::unique_ptr<MatchExpression> _root;

    std::unique_ptr<ParsedProjection> _proj;

    bool _hasNoopExtensions = false;
};

}  // namespace mongo
