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

#include "mongo/db/query_optimizer.h"

#include "mongo/db/query_optimizer_internal.h"
#include "mongo/db/queryoptimizercursorimpl.h"
#include "mongo/db/queryutil.h"

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
