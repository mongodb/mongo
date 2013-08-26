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
        LiteParsedQuery* lpq;
        Status parseStatus = LiteParsedQuery::make(qm, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    // static
    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        CanonicalQuery** out) {
        LiteParsedQuery* lpq;
        // Pass empty sort and projection.
        BSONObj emptyObj;
        Status parseStatus = LiteParsedQuery::make(ns, 0, 0, 0, query, emptyObj, emptyObj, &lpq);
        if (!parseStatus.isOK()) { return parseStatus; }

        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());
        Status initStatus = cq->init(lpq);
        if (!initStatus.isOK()) { return initStatus; }

        *out = cq.release();
        return Status::OK();
    }

    Status CanonicalQuery::init(LiteParsedQuery* lpq) {
        _pq.reset(lpq);

        // Build a parse tree from the BSONObj in the parsed query.
        StatusWithMatchExpression swme = MatchExpressionParser::parse(_pq->getFilter());
        if (!swme.isOK()) { return swme.getStatus(); }

        _root.reset(swme.getValue());
        return Status::OK();
    }

    string CanonicalQuery::toString() const {
        return "Tree: " + _root->toString();
    }

}  // namespace mongo
