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

#include "mongo/db/query/stage_builder.h"

#include "mongo/db/exec/and_hash.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/limit.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/projection.h"
#include "mongo/db/exec/sort.h"
#include "mongo/db/exec/skip.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/namespace_details.h"

namespace mongo {

    PlanStage* buildStages(const string& ns, const QuerySolutionNode* root, WorkingSet* ws) {
        if (STAGE_COLLSCAN == root->getType()) {
            const CollectionScanNode* csn = static_cast<const CollectionScanNode*>(root);
            CollectionScanParams params;
            params.ns = csn->name;
            params.tailable = csn->tailable;
            params.direction = (csn->direction == 1) ? CollectionScanParams::FORWARD
                                                     : CollectionScanParams::BACKWARD;
            return new CollectionScan(params, ws, csn->filter);
        }
        else if (STAGE_IXSCAN == root->getType()) {
            const IndexScanNode* ixn = static_cast<const IndexScanNode*>(root);
            //
            // XXX XXX
            // Given that this grabs data from the catalog, we must do this inside of a lock.
            // We should change this to take a (ns, index key pattern) pair so that the params
            // don't involve any on-disk data, just descriptions thereof.
            // XXX XXX
            //
            IndexScanParams params;
            NamespaceDetails* nsd = nsdetails(ns.c_str());
            if (NULL == nsd) {
                warning() << "Can't ixscan null ns " << ns << endl;
                return NULL;
            }
            int idxNo = nsd->findIndexByKeyPattern(ixn->indexKeyPattern);
            if (-1 == idxNo) {
                warning() << "Can't find idx " << ixn->indexKeyPattern.toString()
                          << "in ns " << ns << endl;
                return NULL;
            }
            params.descriptor = CatalogHack::getDescriptor(nsd, idxNo);
            params.bounds = ixn->bounds;
            params.direction = ixn->direction;
            params.limit = ixn->limit;
            return new IndexScan(params, ws, ixn->filter);
        }
        else if (STAGE_FETCH == root->getType()) {
            const FetchNode* fn = static_cast<const FetchNode*>(root);
            PlanStage* childStage = buildStages(ns, fn->child.get(), ws);
            if (NULL == childStage) { return NULL; }
            return new FetchStage(ws, childStage, fn->filter);
        }
        else if (STAGE_SORT == root->getType()) {
            const SortNode* sn = static_cast<const SortNode*>(root);
            PlanStage* childStage = buildStages(ns, sn->child.get(), ws);
            if (NULL == childStage) { return NULL; }
            SortStageParams params;
            params.pattern = sn->pattern;
            return new SortStage(params, ws, childStage);
        }
        else if (STAGE_PROJECTION == root->getType()) {
            const ProjectionNode* pn = static_cast<const ProjectionNode*>(root);
            QueryProjection* proj;
            if (!QueryProjection::newInclusionExclusion(pn->projection, &proj).isOK()) {
                return NULL;
            }
            PlanStage* childStage = buildStages(ns, pn->child.get(), ws);
            if (NULL == childStage) {
                delete proj;
                return NULL;
            }
            return new ProjectionStage(proj, ws, childStage, NULL);
        }
        else if (STAGE_LIMIT == root->getType()) {
            const LimitNode* ln = static_cast<const LimitNode*>(root);
            PlanStage* childStage = buildStages(ns, ln->child.get(), ws);
            if (NULL == childStage) { return NULL; }
            return new LimitStage(ln->limit, ws, childStage);
        }
        else if (STAGE_SKIP == root->getType()) {
            const SkipNode* sn = static_cast<const SkipNode*>(root);
            PlanStage* childStage = buildStages(ns, sn->child.get(), ws);
            if (NULL == childStage) { return NULL; }
            return new SkipStage(sn->skip, ws, childStage);
        }
        else if (STAGE_AND_HASH == root->getType()) {
            const AndHashNode* ahn = static_cast<const AndHashNode*>(root);
            auto_ptr<AndHashStage> ret(new AndHashStage(ws, ahn->filter));
            for (size_t i = 0; i < ahn->children.size(); ++i) {
                PlanStage* childStage = buildStages(ns, ahn->children[i], ws);
                if (NULL == childStage) { return NULL; }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        else if (STAGE_OR == root->getType()) {
            const OrNode * orn = static_cast<const OrNode*>(root);
            auto_ptr<OrStage> ret(new OrStage(ws, orn->dedup, orn->filter));
            for (size_t i = 0; i < orn->children.size(); ++i) {
                PlanStage* childStage = buildStages(ns, orn->children[i], ws);
                if (NULL == childStage) { return NULL; }
                ret->addChild(childStage);
            }
            return ret.release();
        }
        else {
            stringstream ss;
            root->appendToString(&ss, 0);
            warning() << "Could not build exec tree for node " << ss.str() << endl;
            return NULL;
        }
    }

    //static
    bool StageBuilder::build(const QuerySolution& solution, PlanStage** rootOut,
                             WorkingSet** wsOut) {
        QuerySolutionNode* root = solution.root.get();
        if (NULL == root) { return false; }

        auto_ptr<WorkingSet> ws(new WorkingSet());
        PlanStage* stageRoot = buildStages(solution.ns, root, ws.get());

        if (NULL != stageRoot) {
            *rootOut = stageRoot;
            *wsOut = ws.release();
            return true;
        }
        else {
            return false;
        }
    }

}  // namespace mongo
