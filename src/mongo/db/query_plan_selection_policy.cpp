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

#include "mongo/db/query_plan_selection_policy.h"

#include "mongo/db/queryoptimizer.h"

namespace mongo {

    QueryPlanSelectionPolicy::Any QueryPlanSelectionPolicy::__any;
    const QueryPlanSelectionPolicy& QueryPlanSelectionPolicy::any() { return __any; }
    
    bool QueryPlanSelectionPolicy::IndexOnly::permitPlan( const QueryPlan& plan ) const {
        return !plan.willScanTable();
    }
    QueryPlanSelectionPolicy::IndexOnly QueryPlanSelectionPolicy::__indexOnly;
    const QueryPlanSelectionPolicy& QueryPlanSelectionPolicy::indexOnly() { return __indexOnly; }
    
    bool QueryPlanSelectionPolicy::IdElseNatural::permitPlan( const QueryPlan& plan ) const {
        return !plan.indexed() || plan.index()->isIdIndex();
    }
    BSONObj QueryPlanSelectionPolicy::IdElseNatural::planHint( const StringData& ns ) const {
        NamespaceDetails* nsd = nsdetails( ns );
        if ( !nsd || !nsd->haveIdIndex() ) {
            return BSON( "$hint" << BSON( "$natural" << 1 ) );
        }
        return BSON( "$hint" << nsd->idx( nsd->findIdIndex() ).indexName() );
    }
    QueryPlanSelectionPolicy::IdElseNatural QueryPlanSelectionPolicy::__idElseNatural;
    const QueryPlanSelectionPolicy& QueryPlanSelectionPolicy::idElseNatural() {
        return __idElseNatural;
    }

} // namespace mongo
