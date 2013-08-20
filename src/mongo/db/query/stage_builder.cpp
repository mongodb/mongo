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

#include "mongo/db/query/stage_builder.h"

#include "mongo/db/exec/collection_scan.h"

namespace mongo {

    //static
    bool StageBuilder::build(const QuerySolution& solution, PlanStage** rootOut,
                             WorkingSet** wsOut) {
        QuerySolutionNode* root = solution.root.get();
        if (NULL == root) { return false; }

        if (STAGE_COLLSCAN == root->getType()) {
            const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(root);
            CollectionScanParams params;
            params.ns = csn->name;
            params.tailable = csn->tailable;
            params.direction = (csn->direction == 1) ? CollectionScanParams::FORWARD
                                                     : CollectionScanParams::BACKWARD;
            *wsOut = new WorkingSet();
            *rootOut = new CollectionScan(params, *wsOut, csn->filter);
            return true;
        }
        else {
            return false;
        }
    }

}  // namespace mongo
