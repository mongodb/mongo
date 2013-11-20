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

#include "mongo/db/query/planner_analysis.h"

#include <vector>

#include "mongo/db/jsobj.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/qlog.h"

namespace mongo {

    // static
    QuerySolution* QueryPlannerAnalysis::analyzeDataAccess(const CanonicalQuery& query,
                                                   const QueryPlannerParams& params,
                                                   QuerySolutionNode* solnRoot) {
        auto_ptr<QuerySolution> soln(new QuerySolution());
        soln->filterData = query.getQueryObj();
        verify(soln->filterData.isOwned());
        soln->ns = query.ns();

        solnRoot->computeProperties();

        // solnRoot finds all our results.  Let's see what transformations we must perform to the
        // data.

        // If we're answering a query on a sharded system, we need to drop documents that aren't
        // logically part of our shard (XXX GREG elaborate more precisely)
        if (params.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            // XXX TODO: use params.shardKey to do fetch analysis instead of always fetching.
            if (!solnRoot->fetched()) {
                FetchNode* fetch = new FetchNode();
                fetch->children.push_back(solnRoot);
                solnRoot = fetch;
            }
            ShardingFilterNode* sfn = new ShardingFilterNode();
            sfn->children.push_back(solnRoot);
            solnRoot = sfn;
        }

        bool blockingSort = false;

        // Sort the results, if there is a sort specified.
        if (!query.getParsed().getSort().isEmpty()) {
            const BSONObj& sortObj = query.getParsed().getSort();

            // TODO: We could check sortObj for any projections other than :1 and :-1
            // and short-cut some of this.

            // If the sort is $natural, we ignore it, assuming that the caller has detected that and
            // outputted a collscan to satisfy the desired order.
            BSONElement natural = sortObj.getFieldDotted("$natural");
            if (natural.eoo()) {
                BSONObjSet sorts = solnRoot->getSort();
                // See if solnRoot gives us the sort.  If so, we're done.
                if (sorts.end() == sorts.find(sortObj)) {
                    // Sort is not provided.  See if we provide the reverse of our sort pattern.
                    // If so, we can reverse the scan direction(s).
                    BSONObj reverseSort = QueryPlannerCommon::reverseSortObj(sortObj);
                    if (sorts.end() != sorts.find(reverseSort)) {
                        QueryPlannerCommon::reverseScans(solnRoot);
                        QLOG() << "Reversing ixscan to provide sort.  Result: "
                               << solnRoot->toString() << endl;
                    }
                    else {
                        // If we're not allowed to put a blocking sort in, bail out.
                        if (params.options & QueryPlannerParams::NO_BLOCKING_SORT) {
                            delete solnRoot;
                            return NULL;
                        }

                        // XXX TODO: Can we pull values out of the key and if so in what
                        // cases?  (covered_index_sort_3.js)

                        if (!solnRoot->fetched()) {
                            FetchNode* fetch = new FetchNode();
                            fetch->children.push_back(solnRoot);
                            solnRoot = fetch;
                        }

                        soln->hasSortStage = true;
                        SortNode* sort = new SortNode();
                        sort->pattern = sortObj;
                        sort->query = query.getParsed().getFilter();
                        sort->children.push_back(solnRoot);
                        solnRoot = sort;
                        blockingSort = true;
                    }
                }
            }
        }

        // Project the results.
        if (NULL != query.getLiteProj()) {
            QLOG() << "PROJECTION: fetched status: " << solnRoot->fetched() << endl;
            QLOG() << "PROJECTION: Current plan is:\n" << solnRoot->toString() << endl;
            if (query.getLiteProj()->requiresDocument()) {
                QLOG() << "PROJECTION: claims to require doc adding fetch.\n";
                // If the projection requires the entire document, somebody must fetch.
                if (!solnRoot->fetched()) {
                    FetchNode* fetch = new FetchNode();
                    fetch->children.push_back(solnRoot);
                    solnRoot = fetch;
                }
            }
            else {
                QLOG() << "PROJECTION: requires fields\n";
                vector<string> fields;
                query.getLiteProj()->getRequiredFields(&fields);
                bool covered = true;
                for (size_t i = 0; i < fields.size(); ++i) {
                    if (!solnRoot->hasField(fields[i])) {
                        QLOG() << "PROJECTION: not covered cuz doesn't have field "
                             << fields[i] << endl;
                        covered = false;
                        break;
                    }
                }
                QLOG() << "PROJECTION: is covered?: = " << covered << endl;
                // If any field is missing from the list of fields the projection wants,
                // a fetch is required.
                if (!covered) {
                    FetchNode* fetch = new FetchNode();
                    fetch->children.push_back(solnRoot);
                    solnRoot = fetch;
                }
            }

            // We now know we have whatever data is required for the projection.
            ProjectionNode* projNode = new ProjectionNode();
            projNode->liteProjection = query.getLiteProj();
            projNode->children.push_back(solnRoot);
            projNode->fullExpression = query.root();
            solnRoot = projNode;
        }
        else {
            // If there's no projection, we must fetch, as the user wants the entire doc.
            if (!solnRoot->fetched()) {
                FetchNode* fetch = new FetchNode();
                fetch->children.push_back(solnRoot);
                solnRoot = fetch;
            }
        }

        if (0 != query.getParsed().getSkip()) {
            SkipNode* skip = new SkipNode();
            skip->skip = query.getParsed().getSkip();
            skip->children.push_back(solnRoot);
            solnRoot = skip;
        }

        if (0 != query.getParsed().getNumToReturn() &&
            (blockingSort || !query.getParsed().wantMore())) {

            LimitNode* limit = new LimitNode();
            limit->limit = query.getParsed().getNumToReturn();
            limit->children.push_back(solnRoot);
            solnRoot = limit;
        }

        soln->root.reset(solnRoot);
        return soln.release();
    }

}  // namespace mongo
