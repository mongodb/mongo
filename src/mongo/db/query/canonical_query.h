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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/query/lite_parsed_query.h"

namespace mongo {

    class CanonicalQuery {
    public:
        static Status canonicalize(const QueryMessage& qm, CanonicalQuery** out);

        // This is for testing, when we don't have a QueryMessage.
        static Status canonicalize(const string& ns, const BSONObj& query, CanonicalQuery** out);

        // What namespace is this query over?
        const string& ns() const { return _pq->ns(); }

        //
        // Accessors for the query
        //
        MatchExpression* root() const { return _root.get(); }
        BSONObj getQueryObj() const { return _pq->getFilter(); }
        const LiteParsedQuery& getParsed() const { return *_pq; }

        string toString() const;

    private:
        // You must go through canonicalize to create a CanonicalQuery.
        CanonicalQuery() { }

        scoped_ptr<LiteParsedQuery> _pq;

        // _root points into _pq->getFilter()
        scoped_ptr<MatchExpression> _root;
    };

}  // namespace mongo
