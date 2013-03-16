/**
 *    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/query_plan_selection_policy.h"

namespace mongo {

    class Cursor;
    class ParsedQuery;
    struct QueryPlanSummary;
    
    /**
     * @return a cursor interface to the query optimizer.  The implementation may utilize a
     * single query plan or interleave results from multiple query plans before settling on a
     * single query plan.  Note that the schema of currKey() documents, indexKeyPattern(), the
     * matcher(), and the isMultiKey() nature of the cursor may change over the course of
     * iteration.
     *
     * @param query - Query used to select indexes and populate matchers; not copied if unowned
     * (see bsonobj.h).
     *
     * @param order - Required ordering spec for documents produced by this cursor, empty object
     * default indicates no order requirement.  If no index exists that satisfies the required
     * sort order, an empty shared_ptr is returned unless parsedQuery is also provided.  This is
     * not copied if unowned.
     *
     * @param planPolicy - A policy for selecting query plans - see queryoptimizercursor.h
     *
     * @param parsedQuery - Additional query parameters, as from a client query request.
     *
     * @param requireOrder - If false, the resulting cursor may return results in an order
     * inconsistent with the @param order spec.  See queryoptimizercursor.h for information on
     * handling these results properly.
     *
     * @param singlePlanSummary - Query plan summary information that may be provided when a
     * cursor running a single plan is returned.
     *
     * The returned cursor may @throw inside of advance() or recoverFromYield() in certain error
     * cases, for example if a capped overrun occurred during a yield.  This indicates that the
     * cursor was unable to perform a complete scan.
     *
     * This is a work in progress.  Partial list of features not yet implemented through this
     * interface:
     * 
     * - covered indexes
     * - in memory sorting
     */
    shared_ptr<Cursor> getOptimizedCursor( const StringData& ns,
                                           const BSONObj& query,
                                           const BSONObj& order = BSONObj(),
                                           const QueryPlanSelectionPolicy& planPolicy =
                                               QueryPlanSelectionPolicy::any(),
                                           const shared_ptr<const ParsedQuery>& parsedQuery =
                                               shared_ptr<const ParsedQuery>(),
                                           bool requireOrder = true,
                                           QueryPlanSummary* singlePlanSummary = NULL );

    /**
     * @return a single cursor that may work well for the given query.  A $or style query will
     * produce a single cursor, not a MultiCursor.
     * It is possible no cursor is returned if the sort is not supported by an index.  Clients
     * are responsible for checking this if they are not sure an index for a sort exists, and
     * defaulting to a non-sort if no suitable indices exist.
     */
    shared_ptr<Cursor> getBestGuessCursor( const char* ns,
                                           const BSONObj& query,
                                           const BSONObj& sort );

} // namespace mongo
