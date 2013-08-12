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
 */

#include "mongo/db/query/canonical_query.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression_parser.h"

namespace mongo {

    // static
    Status CanonicalQuery::canonicalize(const QueryMessage& qm, CanonicalQuery** out) {
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());

        // Parse the query.
        LiteParsedQuery* lpq;
        Status parseStatus = LiteParsedQuery::make(qm, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }
        cq->_pq.reset(lpq);

        // Build a parse tree from the BSONObj in the parsed query.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(cq->_pq->getFilter());
        if (!swme.isOK()) { return swme.getStatus(); }

        cq->_root.reset(swme.getValue());
        *out = cq.release();
        return Status::OK();
    }

    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        CanonicalQuery** out) {
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());

        LiteParsedQuery* lpq;
        Status parseStatus = LiteParsedQuery::make(ns, 0, 0, 0, query, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }
        cq->_pq.reset(lpq);

        StatusWithMatchExpression swme = MatchExpressionParser::parse(cq->_pq->getFilter());
        if (!swme.isOK()) { return swme.getStatus(); }

        cq->_root.reset(swme.getValue());
        *out = cq.release();
        return Status::OK();
    }

    string CanonicalQuery::toString() const {
        return "Tree: " + _root->toString();
    }

}  // namespace mongo
