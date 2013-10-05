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

#include "mongo/db/query/explain_plan.h"

#include "mongo/db/query/stage_types.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    Status explainPlan(const PlanStageStats& stats, TypeExplain** explain, bool fullDetails) {
        auto_ptr<TypeExplain> res(new TypeExplain);

        // Descend the plan looking for structural properties:
        // + is is a collection scan or a an index scan?
        // + if the latter, was it covered?
        // + was a sort necessary?
        //
        // TODO: For now, we assume that at most one index is used in a plan
        bool covered = true;
        bool sortPresent = false;
        const PlanStageStats* root = &stats;
        const PlanStageStats* leaf = root;
        while (leaf->children.size() > 0) {

            // We're not failing a plan with multiple children (e.g., OR) but we're not
            // yet outputing it properly.
            // TODO: "clauses" field of the explain structure.
            if (leaf->children.size() > 1) {
                res->setCursor("Complex Plan");
                res->setNScanned(0);
                res->setNScannedObjects(0);
                *explain = res.release();
                return Status::OK();
            }

            if (leaf->stageType == STAGE_FETCH) {
                covered = false;
            }
            if (leaf->stageType == STAGE_SORT) {
                sortPresent = true;
            }
            leaf = leaf->children[0];
        }

        // How many documents did the query return?
        res->setN(root->common.advanced);

        // Accounting for 'nscanned' and 'nscannedObjects' is specific to the kind of leaf:
        //
        // + on collection scan, both are the same; all the documents retrieved were
        //   fetched in practice. To get how many documents were retrieved, one simply
        //   looks at the number of 'advanced' in the stats.
        //
        // + on an index scan, we'd neeed to look into the index scan cursor to extract the
        //   number of keys that cursor retrieved, and into the stage's stats 'advanced' for
        //   nscannedObjects', which would be the number of keys that survived the IXSCAN
        //   filter. Those keys would have been FETCH-ed, if a fetch is present.
        if (leaf->stageType == STAGE_COLLSCAN) {
            CollectionScanStats* csStats = static_cast<CollectionScanStats*>(leaf->specific.get());
            res->setCursor("BasicCursor");
            res->setNScanned(csStats->docsTested);
            res->setNScannedObjects(csStats->docsTested);

            if (fullDetails) {
                res->setIsMultiKey(false);
                res->setIndexOnly(false);
            }
        }
        else if (leaf->stageType == STAGE_GEO_NEAR_2DSPHERE) {
            // TODO: This is kind of a lie for STAGE_GEO_NEAR_2DSPHERE.
            res->setCursor("S2NearCursor");
            // The first work() is an init.  Every subsequent work examines a document.
            res->setNScanned(leaf->common.works);
            res->setNScannedObjects(leaf->common.works);

            if (fullDetails) {
                res->setIsMultiKey(false);
                res->setIndexOnly(false);
            }
        }
        else if (leaf->stageType == STAGE_IXSCAN) {
            IndexScanStats* indexStats = static_cast<IndexScanStats*>(leaf->specific.get());
            dassert(indexStats);
            res->setCursor(indexStats->indexType + " " + indexStats->indexName);
            res->setNScanned(indexStats->keysExamined);

            // If we're covered, that is, no FETCH is present, then, by definition,
            // nScannedObject would be zero because no full document would have been fetched
            // from disk.
            if (covered) {
                res->setNScannedObjects(0);
            }
            else {
                res->setNScannedObjects(leaf->common.advanced);
            }

            res->setIndexBounds(indexStats->indexBounds);

            if (fullDetails) {
                res->setIsMultiKey(indexStats->isMultiKey);
                res->setIndexOnly(covered);
            }
        }
        else {
            return Status(ErrorCodes::InternalError, "cannot interpret execution plan");
        }

        // Statistics for the plan (appear only in a detailed mode)
        if (fullDetails) {
            res->setScanAndOrder(sortPresent);
            res->setNYields(root->common.yields);
        }

        *explain = res.release();
        return Status::OK();
    }

} // namespace mongo
