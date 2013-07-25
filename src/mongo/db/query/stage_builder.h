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

#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/and_sorted.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/query_solution.h"

#pragma once

namespace mongo {

    /**
     * The StageBuilder converts a QuerySolution to an executable tree of PlanStage(s).
     */
    class StageBuilder {
    public:
        static bool build(const QuerySolution& solution, PlanStage** rootOut, WorkingSet** wsOut) {
            return false;
            // TODO: Implement.
        }
    };

}  // namespace mongo
