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
    Status CanonicalQuery::canonicalize(QueryMessage& qm, CanonicalQuery** out) {
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());

        // TODO: ParsedQuery throws.  Fix it to return error.
        cq->_pq.reset(new ParsedQuery(qm));

        StatusWithMatchExpression swme = MatchExpressionParser::parse(cq->_pq->getFilter());
        if (!swme.isOK()) {
            return swme.getStatus();
        }

        cq->_root.reset(swme.getValue());
        *out = cq.release();
        return Status::OK();
    }

    Status CanonicalQuery::canonicalize(const string& ns, const BSONObj& query,
                                        CanonicalQuery** out) {
        auto_ptr<CanonicalQuery> cq(new CanonicalQuery());

        cq->_pq.reset(new ParsedQuery(ns.c_str(), 0, 0, 0, query, BSONObj()));

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
