/**
 *    Copyright (C) 2011 10gen Inc.
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

#include "mongo/db/query_optimizer.h"

#include "mongo/db/query_optimizer_internal.h"
#include "mongo/db/queryoptimizercursorimpl.h"

namespace mongo {

    shared_ptr<Cursor> getOptimizedCursor( const StringData& ns,
                                           const BSONObj& query,
                                           const BSONObj& order,
                                           const QueryPlanSelectionPolicy& planPolicy,
                                           const shared_ptr<const ParsedQuery>& parsedQuery,
                                           bool requireOrder,
                                           QueryPlanSummary* singlePlanSummary ) {
        CursorGenerator generator( ns,
                                   query,
                                   order,
                                   planPolicy,
                                   parsedQuery,
                                   requireOrder,
                                   singlePlanSummary );
        return generator.generate();
    }

    shared_ptr<Cursor> getBestGuessCursor( const char* ns,
                                           const BSONObj& query,
                                           const BSONObj& sort ) {

        auto_ptr<FieldRangeSetPair> frsp( new FieldRangeSetPair( ns, query, true ) );
        auto_ptr<FieldRangeSetPair> origFrsp( new FieldRangeSetPair( *frsp ) );

        scoped_ptr<QueryPlanSet> qps( QueryPlanSet::make( ns,
                                                          frsp,
                                                          origFrsp,
                                                          query,
                                                          sort,
                                                          shared_ptr<const ParsedQuery>(),
                                                          BSONObj(),
                                                          QueryPlanGenerator::UseIfInOrder,
                                                          BSONObj(),
                                                          BSONObj(),
                                                          true ) );
        QueryPlanSet::QueryPlanPtr qpp = qps->getBestGuess();
        if( ! qpp.get() ) return shared_ptr<Cursor>();

        shared_ptr<Cursor> ret = qpp->newCursor();

        // If we don't already have a matcher, supply one.
        if ( !query.isEmpty() && ! ret->matcher() ) {
            ret->setMatcher( qpp->matcher() );
        }
        return ret;
    }
    
} // namespace mongo;
