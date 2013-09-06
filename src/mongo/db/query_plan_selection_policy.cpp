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

#include "mongo/db/query_plan_selection_policy.h"

#include "mongo/db/query_optimizer_internal.h"

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
