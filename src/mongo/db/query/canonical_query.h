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
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/parsed_projection.h"
#include "mongo/db/query/query_request.h"

namespace mongo {

class OperationContext;

class CanonicalQuery {
public:
    /**
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * 'txn' must point to a valid OperationContext, but 'txn' does not need to outlive the returned
     * CanonicalQuery.
     *
     * Used for legacy find through the OP_QUERY message.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        OperationContext* txn,
        const QueryMessage& qm,
        const ExtensionsCallback& extensionsCallback);

    /**
     * If parsing succeeds, returns a std::unique_ptr<CanonicalQuery> representing the parsed
     * query (which will never be NULL).  If parsing fails, returns an error Status.
     *
     * 'txn' must point to a valid OperationContext, but 'txn' does not need to outlive the returned
     *  CanonicalQuery.
     */
    static StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(
        OperationContext* txn, std::unique_ptr<QueryRequest> qr, const ExtensionsCallback&);

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
        OperationContext* txn,
        const CanonicalQuery& baseQuery,
        MatchExpression* root,
        const ExtensionsCallback& extensionsCallback);

    /**
     * Returns true if "query" describes an exact-match query on _id, possibly with
     * the $isolated/$atomic modifier.
     */
    static bool isSimpleIdQuery(const BSONObj& query);

    const NamespaceString& nss() const {
        return _qr->nss();
    }
    const std::string& ns() const {
        return _qr->nss().ns();
    }

    //
    // Accessors for the query
    //
    MatchExpression* root() const {
        return _root.get();
    }
    BSONObj getQueryObj() const {
        return _qr->getFilter();
    }
    const QueryRequest& getQueryRequest() const {
        return *_qr;
    }
    const ParsedProjection* getProj() const {
        return _proj.get();
    }
    const CollatorInterface* getCollator() const {
        return _collator.get();
    }

    /**
     * Sets this CanonicalQuery's collator, and sets the collator on this CanonicalQuery's match
     * expression tree.
     *
     * This setter can be used to override the collator that was created from the query request
     * during CanonicalQuery construction.
     */
    void setCollator(std::unique_ptr<CollatorInterface> collator);

    // Debugging
    std::string toString() const;
    std::string toStringShort() const;

    /**
     * Validates match expression, checking for certain
     * combinations of operators in match expression and
     * query options in QueryRequest.
     * Since 'root' is derived from 'filter' in QueryRequest,
     * 'filter' is not validated.
     *
     * TODO: Move this to query_validator.cpp
     */
    static Status isValid(MatchExpression* root, const QueryRequest& parsed);

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

    /**
     * Returns true if the query this CanonicalQuery was parsed from included a $isolated/$atomic
     * operator.
     */
    bool isIsolated() const {
        return _isIsolated;
    }

private:
    // You must go through canonicalize to create a CanonicalQuery.
    CanonicalQuery() {}

    Status init(std::unique_ptr<QueryRequest> qr,
                const ExtensionsCallback& extensionsCallback,
                MatchExpression* root,
                std::unique_ptr<CollatorInterface> collator);

    std::unique_ptr<QueryRequest> _qr;

    // _root points into _qr->getFilter()
    std::unique_ptr<MatchExpression> _root;

    std::unique_ptr<ParsedProjection> _proj;

    std::unique_ptr<CollatorInterface> _collator;

    bool _hasNoopExtensions = false;

    bool _isIsolated;
};

}  // namespace mongo
