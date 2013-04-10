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

#include "mongo/db/projection.h"

namespace mongo {

    class FieldRangeSet;
    
    /**
     * A partial description of a QueryPlan that provides access to relevant plan attributes outside
     * of query optimizer internal code.
     */
    struct QueryPlanSummary {
        QueryPlanSummary() :
            scanAndOrderRequired() {
        }

        /**
         * The 'fieldRangeMulti' attribute is required, and its presence indicates the object has
         * been configured with a query plan.
         */
        bool valid() const { return fieldRangeSetMulti; }

        // A description of the valid values for the fields of a query, in the context of a multikey
        // index or in memory sort.
        shared_ptr<FieldRangeSet> fieldRangeSetMulti;

        // A helper object used to implement covered index queries.  This attribute is only non-NULL
        // if the query plan supports covered index queries.
        shared_ptr<Projection::KeyOnly> keyFieldsOnly;

        // True if the query plan results must be reordered to match a requested ordering.
        bool scanAndOrderRequired;
    };

} // namespace mongo
