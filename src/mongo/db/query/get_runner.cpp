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

#include "mongo/db/query/get_runner.h"

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/query/cached_plan_runner.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/eof_runner.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/idhack_runner.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/s/d_logic.h"

namespace mongo {

    static bool canUseIDHack(const CanonicalQuery& query) {
        return !query.getParsed().isExplain()
            && !query.getParsed().showDiskLoc()
            && CanonicalQuery::isSimpleIdQuery(query.getParsed().getFilter())
            && !query.getParsed().hasOption(QueryOption_CursorTailable);
    }

    // static
    void filterAllowedIndexEntries(const AllowedIndices& allowedIndices,
                                   std::vector<IndexEntry>* indexEntries) {
        invariant(indexEntries);

        // Filter index entries
        // Check BSON objects in AllowedIndices::_indexKeyPatterns against IndexEntry::keyPattern.
        // Removes IndexEntrys that do not match _indexKeyPatterns.
        std::vector<IndexEntry> temp;
        for (std::vector<IndexEntry>::const_iterator i = indexEntries->begin();
             i != indexEntries->end(); ++i) {
            const IndexEntry& indexEntry = *i;
            for (std::vector<BSONObj>::const_iterator j = allowedIndices.indexKeyPatterns.begin();
                 j != allowedIndices.indexKeyPatterns.end(); ++j) {
                const BSONObj& index = *j;
                // Copy index entry to temp vector if found in query settings.
                if (0 == indexEntry.keyPattern.woCompare(index)) {
                    temp.push_back(indexEntry);
                    break;
                }
            }
        }

        // Update results.
        temp.swap(*indexEntries);
    }

    /**
     * For a given query, get a runner.  The runner could be a SingleSolutionRunner, a
     * CachedQueryRunner, or a MultiPlanRunner, depending on the cache/query solver/etc.
     */
    Status getRunner(CanonicalQuery* rawCanonicalQuery,
                     Runner** out, size_t plannerOptions) {
        verify(rawCanonicalQuery);
        Database* db = cc().database();
        verify(db);
        return getRunner(db->getCollection(rawCanonicalQuery->ns()),
                         rawCanonicalQuery,
                         out,
                         plannerOptions);
    }

    Status getRunner(Collection* collection,
                     const std::string& ns,
                     const BSONObj& unparsedQuery,
                     Runner** outRunner,
                     CanonicalQuery** outCanonicalQuery,
                     size_t plannerOptions) {

        if (!collection) {
            *outCanonicalQuery = NULL;
            *outRunner = new EOFRunner(NULL, ns);
            return Status::OK();
        }
        if (!CanonicalQuery::isSimpleIdQuery(unparsedQuery) ||
            !collection->getIndexCatalog()->findIdIndex()) {

            Status status = CanonicalQuery::canonicalize(
                    collection->ns(),
                    unparsedQuery,
                    outCanonicalQuery);
            if (!status.isOK())
                return status;
            return getRunner(collection, *outCanonicalQuery, outRunner, plannerOptions);
        }

        *outCanonicalQuery = NULL;
        *outRunner = new IDHackRunner(collection, unparsedQuery["_id"].wrap());
        return Status::OK();
    }

    /**
     * For a given query, get a runner.  The runner could be a SingleSolutionRunner, a
     * CachedQueryRunner, or a MultiPlanRunner, depending on the cache/query solver/etc.
     */
    Status getRunner(Collection* collection, CanonicalQuery* rawCanonicalQuery,
                     Runner** out, size_t plannerOptions) {

        verify(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        // This can happen as we're called by internal clients as well.
        if (NULL == collection) {
            const string& ns = canonicalQuery->ns();
            *out = new EOFRunner(canonicalQuery.release(), ns);
            return Status::OK();
        }

        // If we have an _id index we can use the idhack runner.
        if (canUseIDHack(*canonicalQuery) && collection->getIndexCatalog()->findIdIndex()) {
            *out = new IDHackRunner(collection, canonicalQuery.release());
            return Status::OK();
        }

        // If it's not NULL, we may have indices.  Access the catalog and fill out IndexEntry(s)
        QueryPlannerParams plannerParams;

        IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                       desc->isMultikey(),
                                                       desc->isSparse(),
                                                       desc->indexName(),
                                                       desc->infoObj()));
        }

        // If query supports admin hint, filter params.indices by indexes in query settings.
        QuerySettings* querySettings = collection->infoCache()->getQuerySettings();
        AllowedIndices* allowedIndicesRaw;

        // Filter index catalog if admin hint is specified for query.
        // Also, signal to planner that application hint should be ignored.
        if (querySettings->getAllowedIndices(*canonicalQuery, &allowedIndicesRaw)) {
            boost::scoped_ptr<AllowedIndices> allowedIndices(allowedIndicesRaw);
            filterAllowedIndexEntries(*allowedIndices, &plannerParams.indices);
            plannerParams.adminHintApplied = true;
        }

        // Tailable: If the query requests tailable the collection must be capped.
        if (canonicalQuery->getParsed().hasOption(QueryOption_CursorTailable)) {
            if (!collection->isCapped()) {
                return Status(ErrorCodes::BadValue,
                              "error processing query: " + canonicalQuery->toString() +
                              " tailable cursor requested on non capped collection");
            }

            // If a sort is specified it must be equal to expectedSort.
            const BSONObj expectedSort = BSON("$natural" << 1);
            const BSONObj& actualSort = canonicalQuery->getParsed().getSort();
            if (!actualSort.isEmpty() && !(actualSort == expectedSort)) {
                return Status(ErrorCodes::BadValue,
                              "error processing query: " + canonicalQuery->toString() +
                              " invalid sort specified for tailable cursor: "
                              + actualSort.toString());
            }
        }

        // Process the planning options.
        plannerParams.options = plannerOptions;
        if (storageGlobalParams.noTableScan) {
            const string& ns = canonicalQuery->ns();
            // There are certain cases where we ignore this restriction:
            bool ignore = canonicalQuery->getQueryObj().isEmpty()
                          || (string::npos != ns.find(".system."))
                          || (0 == ns.find("local."));
            if (!ignore) {
                plannerParams.options |= QueryPlannerParams::NO_TABLE_SCAN;
            }
        }

        if (!(plannerParams.options & QueryPlannerParams::NO_TABLE_SCAN)) {
            plannerParams.options |= QueryPlannerParams::INCLUDE_COLLSCAN;
        }

        // If the caller wants a shard filter, make sure we're actually sharded.
        if (plannerParams.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
            CollectionMetadataPtr collMetadata =
                shardingState.getCollectionMetadata(canonicalQuery->ns());

            if (collMetadata) {
                plannerParams.shardKey = collMetadata->getKeyPattern();
            }
            else {
                // If there's no metadata don't bother w/the shard filter since we won't know what
                // the key pattern is anyway...
                plannerParams.options &= ~QueryPlannerParams::INCLUDE_SHARD_FILTER;
            }
        }

        // Try to look up a cached solution for the query.
        //
        // Skip cache look up for non-cacheable queries.
        // See PlanCache::shouldCacheQuery()
        //
        // TODO: Can the cache have negative data about a solution?
        CachedSolution* rawCS;
        if (PlanCache::shouldCacheQuery(*canonicalQuery) &&
            collection->infoCache()->getPlanCache()->get(*canonicalQuery, &rawCS).isOK()) {
            // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
            boost::scoped_ptr<CachedSolution> cs(rawCS);
            QuerySolution *qs, *backupQs;
            Status status = QueryPlanner::planFromCache(*canonicalQuery, plannerParams, *cs,
                                                        &qs, &backupQs);
            if (status.isOK()) {
                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*qs, &root, &ws));
                CachedPlanRunner* cpr = new CachedPlanRunner(collection,
                                                             canonicalQuery.release(), qs,
                                                             root, ws);

                if (NULL != backupQs) {
                    WorkingSet* backupWs;
                    PlanStage* backupRoot;
                    verify(StageBuilder::build(*backupQs, &backupRoot, &backupWs));
                    cpr->setBackupPlan(backupQs, backupRoot, backupWs);
                }

                *out = cpr;
                return Status::OK();
            }
        }

        plannerParams.options |= QueryPlannerParams::INDEX_INTERSECTION;
        plannerParams.options |= QueryPlannerParams::KEEP_MUTATIONS;

        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          "error processing query: " + canonicalQuery->toString() +
                          " planner returned error: " + status.reason());
        }

        /*
        for (size_t i = 0; i < solutions.size(); ++i) {
            QLOG() << "solution " << i << " is " << solutions[i]->toString() << endl;
        }
        */

        // We cannot figure out how to answer the query.  Should this ever happen?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                          << "error processing query: "
                          << canonicalQuery->toString()
                          << " No query solutions");
        }

        if (1 == solutions.size()) {
            // Only one possible plan.  Run it.  Build the stages from the solution.
            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*solutions[0], &root, &ws));

            // And, run the plan.
            *out = new SingleSolutionRunner(collection,
                                            canonicalQuery.release(),solutions[0], root, ws);
            return Status::OK();
        }
        else {
            // Many solutions.  Let the MultiPlanRunner pick the best, update the cache, and so on.
            auto_ptr<MultiPlanRunner> mpr(new MultiPlanRunner(collection,canonicalQuery.release()));
            for (size_t i = 0; i < solutions.size(); ++i) {
                WorkingSet* ws;
                PlanStage* root;
                if (solutions[i]->cacheData.get()) {
                    solutions[i]->cacheData->adminHintApplied = plannerParams.adminHintApplied;
                }
                verify(StageBuilder::build(*solutions[i], &root, &ws));
                // Takes ownership of all arguments.
                mpr->addPlan(solutions[i], root, ws);
            }
            *out = mpr.release();
            return Status::OK();
        }
    }

    //
    // Distinct hack
    //

    /**
     * If possible, turn the provided QuerySolution into a QuerySolution that uses a DistinctNode
     * to provide results for the distinct command.
     *
     * If the provided solution could be mutated successfully, returns true, otherwise returns
     * false.
     */
    bool turnIxscanIntoDistinctIxscan(QuerySolution* soln, const string& field) {
        QuerySolutionNode* root = soln->root.get();

        // We're looking for a project on top of an ixscan.
        if (STAGE_PROJECTION == root->getType() && (STAGE_IXSCAN == root->children[0]->getType())) {
            IndexScanNode* isn = static_cast<IndexScanNode*>(root->children[0]);

            // An additional filter must be applied to the data in the key, so we can't just skip
            // all the keys with a given value; we must examine every one to find the one that (may)
            // pass the filter.
            if (NULL != isn->filter.get()) {
                return false;
            }

            // We only set this when we have special query modifiers (.max() or .min()) or other
            // special cases.  Don't want to handle the interactions between those and distinct.
            // Don't think this will ever really be true but if it somehow is, just ignore this
            // soln.
            if (isn->bounds.isSimpleRange) {
                return false;
            }

            // Make a new DistinctNode.  We swap this for the ixscan in the provided solution.
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = isn->indexKeyPattern;
            dn->direction = isn->direction;
            dn->bounds = isn->bounds;

            // Figure out which field we're skipping to the next value of.  TODO: We currently only
            // try to distinct-hack when there is an index prefixed by the field we're distinct-ing
            // over.  Consider removing this code if we stick with that policy.
            dn->fieldNo = 0;
            BSONObjIterator it(isn->indexKeyPattern);
            while (it.more()) {
                if (field == it.next().fieldName()) {
                    break;
                }
                dn->fieldNo++;
            }

            // Delete the old index scan, set the child of project to the fast distinct scan.
            delete root->children[0];
            root->children[0] = dn;
            return true;
        }

        return false;
    }

    Status getRunnerDistinct(Collection* collection,
                             const BSONObj& query,
                             const string& field,
                             Runner** out) {

        Database* db = cc().database();
        verify(db);

        // This should'a been checked by the distinct command.
        verify(collection);

        // TODO: check for idhack here?

        // When can we do a fast distinct hack?
        // 1. There is a plan with just one leaf and that leaf is an ixscan.
        // 2. The ixscan indexes the field we're interested in.
        // 2a: We are correct if the index contains the field but for now we look for prefix.
        // 3. The query is covered/no fetch.
        //
        // We go through normal planning (with limited parameters) to see if we can produce
        // a soln with the above properties.

        QueryPlannerParams plannerParams;
        plannerParams.options = QueryPlannerParams::NO_TABLE_SCAN;

        IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
        while (ii.more()) {
            const IndexDescriptor* desc = ii.next();
            // The distinct hack can work if any field is in the index but it's not always clear
            // if it's a win unless it's the first field.
            if (desc->keyPattern().firstElement().fieldName() == field) {
                plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                           desc->isMultikey(),
                                                           desc->isSparse(),
                                                           desc->indexName(),
                                                           desc->infoObj()));
            }
        }

        // We only care about the field that we're projecting over.  Have to drop the _id field
        // explicitly because those are .find() semantics.
        //
        // Applying a projection allows the planner to try to give us covered plans.
        BSONObj projection;
        if ("_id" == field) {
            projection = BSON("_id" << 1);
        }
        else {
            projection = BSON("_id" << 0 << field << 1);
        }

        // Apply a projection of the key.  Empty BSONObj() is for the sort.
        CanonicalQuery* cq;
        Status status = CanonicalQuery::canonicalize(collection->ns().ns(), query, BSONObj(), projection, &cq);
        if (!status.isOK()) {
            return status;
        }

        // No index has the field we're looking for.  Punt to normal planning.
        if (plannerParams.indices.empty()) {
            // Takes ownership of cq.
            return getRunner(cq, out);
        }

        // If we're here, we have an index prefixed by the field we're distinct-ing over.

        // If there's no query, we can just distinct-scan one of the indices.
        if (query.isEmpty()) {
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = plannerParams.indices[0].keyPattern;
            dn->direction = 1;
            IndexBoundsBuilder::allValuesBounds(dn->indexKeyPattern, &dn->bounds);
            dn->fieldNo = 0;

            QueryPlannerParams params;

            // Takes ownership of 'dn'.
            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(*cq, params, dn);
            verify(soln);

            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*soln, &root, &ws));
            *out = new SingleSolutionRunner(collection, cq, soln, root, ws);
            return Status::OK();
        }

        // See if we can answer the query in a fast-distinct compatible fashion.
        vector<QuerySolution*> solutions;
        status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        if (!status.isOK()) {
            return getRunner(cq, out);
        }

        // XXX: why do we need to do this?  planner should prob do this internally
        cq->root()->resetTag();

        // We look for a solution that has an ixscan we can turn into a distinctixscan
        for (size_t i = 0; i < solutions.size(); ++i) {
            if (turnIxscanIntoDistinctIxscan(solutions[i], field)) {
                // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                for (size_t j = 0; j < solutions.size(); ++j) {
                    if (j != i) {
                        delete solutions[j];
                    }
                }

                // Build and return the SSR over solutions[i].
                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*solutions[i], &root, &ws));
                *out = new SingleSolutionRunner(collection, cq, solutions[i], root, ws);
                return Status::OK();
            }
        }

        // If we're here, the planner made a soln with the restricted index set but we couldn't
        // translate any of them into a distinct-compatible soln.  So, delete the solutions and just
        // go through normal planning.
        for (size_t i = 0; i < solutions.size(); ++i) {
            delete solutions[i];
        }

        return getRunner(cq, out);
    }

    ScopedRunnerRegistration::ScopedRunnerRegistration(Runner* runner)
        : _runner(runner) {
        // Collection can be null for EOFRunner, or other places where registration is not needed
        if ( _runner->collection() )
            _runner->collection()->cursorCache()->registerRunner( runner );
    }

    ScopedRunnerRegistration::~ScopedRunnerRegistration() {
        if ( _runner->collection() )
            _runner->collection()->cursorCache()->deregisterRunner( _runner );
    }

}  // namespace mongo
