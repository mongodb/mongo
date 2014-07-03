/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
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

// THIS FILE IS DEPRECATED -- replaced by get_executor.cpp

#include "mongo/platform/basic.h"

#include "mongo/db/query/get_runner.h"

#include <limits>

#include "mongo/base/parse_number.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/eof.h"
#include "mongo/db/exec/idhack.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/eof_runner.h"
#include "mongo/db/query/explain_plan.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/idhack_runner.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/planner_access.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/single_solution_runner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/query/subplan_runner.h"
#include "mongo/db/index_names.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/s/d_logic.h"
#include "mongo/util/log.h"

namespace mongo {

    MONGO_LOG_DEFAULT_COMPONENT_FILE(::mongo::logger::LogComponent::kQuery);

    Status getRunner(Collection* collection,
                     const std::string& ns,
                     const BSONObj& unparsedQuery,
                     Runner** outRunner,
                     CanonicalQuery** outCanonicalQuery,
                     size_t plannerOptions) {

        if (!collection) {
            LOG(2) << "Collection " << ns << " does not exist."
                   << " Using EOF runner: " << unparsedQuery.toString();
            *outCanonicalQuery = NULL;
            *outRunner = new EOFRunner(NULL, ns);
            return Status::OK();
        }

        if (!CanonicalQuery::isSimpleIdQuery(unparsedQuery) ||
            !collection->getIndexCatalog()->findIdIndex()) {

            const WhereCallbackReal whereCallback(collection->ns().db());
            Status status = CanonicalQuery::canonicalize(
                        collection->ns(), unparsedQuery, outCanonicalQuery, whereCallback);
            if (!status.isOK())
                return status;
            return getRunner(collection, *outCanonicalQuery, outRunner, plannerOptions);
        }

        LOG(2) << "Using idhack: " << unparsedQuery.toString();

        *outCanonicalQuery = NULL;
        *outRunner = new IDHackRunner(collection, unparsedQuery["_id"].wrap());
        return Status::OK();
    }

    namespace {
        // The body is below in the "count hack" section but getRunner calls it.
        bool turnIxscanIntoCount(QuerySolution* soln);
    }  // namespace


    /**
     * For a given query, get a runner.
     */
    Status getRunner(Collection* collection,
                     CanonicalQuery* rawCanonicalQuery,
                     Runner** out,
                     size_t plannerOptions) {

        verify(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        // This can happen as we're called by internal clients as well.
        if (NULL == collection) {
            const string& ns = canonicalQuery->ns();
            LOG(2) << "Collection " << ns << " does not exist."
                   << " Using EOF runner: " << canonicalQuery->toStringShort();
            *out = new EOFRunner(canonicalQuery.release(), ns);
            return Status::OK();
        }

        // If we have an _id index we can use the idhack runner.
        if (IDHackStage::supportsQuery(*canonicalQuery) &&
            collection->getIndexCatalog()->findIdIndex()) {
            LOG(2) << "Using idhack: " << canonicalQuery->toStringShort();
            *out = new IDHackRunner(collection, canonicalQuery.release());
            return Status::OK();
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

        // Fill out the planning params.  We use these for both cached solutions and non-cached.
        QueryPlannerParams plannerParams;
        plannerParams.options = plannerOptions;
        fillOutPlannerParams(collection, rawCanonicalQuery, &plannerParams);

        // Try to look up a cached solution for the query.

        CachedSolution* rawCS;
        if (PlanCache::shouldCacheQuery(*canonicalQuery) &&
            collection->infoCache()->getPlanCache()->get(*canonicalQuery, &rawCS).isOK()) {
            // We have a CachedSolution.  Have the planner turn it into a QuerySolution.
            boost::scoped_ptr<CachedSolution> cs(rawCS);
            QuerySolution *qs, *backupQs;
            QuerySolution*& chosenSolution=qs; // either qs or backupQs
            Status status = QueryPlanner::planFromCache(*canonicalQuery, plannerParams, *cs,
                                                        &qs, &backupQs);

            if (status.isOK()) {
                // the working set will be shared by the root and backupRoot plans
                // and owned by the containing single-solution-runner
                //
                WorkingSet* sharedWs = new WorkingSet();

                PlanStage *root, *backupRoot=NULL;
                verify(StageBuilder::build(collection, *qs, sharedWs, &root));
                if ((plannerParams.options & QueryPlannerParams::PRIVATE_IS_COUNT)
                    && turnIxscanIntoCount(qs)) {
                    LOG(2) << "Using fast count: " << canonicalQuery->toStringShort()
                           << ", planSummary: " << getPlanSummary(*qs);

                    if (NULL != backupQs) {
                        delete backupQs;
                    }
                }
                else if (NULL != backupQs) {
                    verify(StageBuilder::build(collection, *backupQs, sharedWs, &backupRoot));
                }

                // add a CachedPlanStage on top of the previous root
                root = new CachedPlanStage(collection, rawCanonicalQuery, root, backupRoot);

                *out = new SingleSolutionRunner(collection,
                                                canonicalQuery.release(),
                                                chosenSolution, root, sharedWs);
                return Status::OK();
            }
        }

        if (internalQueryPlanOrChildrenIndependently
            && SubplanRunner::canUseSubplanRunner(*canonicalQuery)) {

            QLOG() << "Running query as sub-queries: " << canonicalQuery->toStringShort();
            LOG(2) << "Running query as sub-queries: " << canonicalQuery->toStringShort();

            SubplanRunner* runner;
            Status runnerStatus = SubplanRunner::make(collection, plannerParams,
                                                      canonicalQuery.release(), &runner);
            if (!runnerStatus.isOK()) {
                return runnerStatus;
            }

            *out = runner;
            return Status::OK();
        }

        return getRunnerAlwaysPlan(collection, canonicalQuery.release(), plannerParams, out);
    }

    Status getRunnerAlwaysPlan(Collection* collection,
                               CanonicalQuery* rawCanonicalQuery,
                               const QueryPlannerParams& plannerParams,
                               Runner** out) {
        invariant(collection);
        invariant(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          "error processing query: " + canonicalQuery->toString() +
                          " planner returned error: " + status.reason());
        }

        // We cannot figure out how to answer the query.  Perhaps it requires an index
        // we do not have?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue,
                          str::stream()
                          << "error processing query: "
                          << canonicalQuery->toString()
                          << " No query solutions");
        }

        // See if one of our solutions is a fast count hack in disguise.
        if (plannerParams.options & QueryPlannerParams::PRIVATE_IS_COUNT) {
            for (size_t i = 0; i < solutions.size(); ++i) {
                if (turnIxscanIntoCount(solutions[i])) {
                    // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                    for (size_t j = 0; j < solutions.size(); ++j) {
                        if (j != i) {
                            delete solutions[j];
                        }
                    }

                    LOG(2) << "Using fast count: " << canonicalQuery->toStringShort()
                           << ", planSummary: " << getPlanSummary(*solutions[i]);

                    // We're not going to cache anything that's fast count.
                    WorkingSet* ws = new WorkingSet();
                    PlanStage* root;
                    verify(StageBuilder::build(collection, *solutions[i], ws, &root));
                    *out = new SingleSolutionRunner(collection,
                                                    canonicalQuery.release(),
                                                    solutions[i],
                                                    root,
                                                    ws);
                    return Status::OK();
                }
            }
        }

        if (1 == solutions.size()) {
            LOG(2) << "Only one plan is available; it will be run but will not be cached. "
                   << canonicalQuery->toStringShort()
                   << ", planSummary: " << getPlanSummary(*solutions[0]);

            // Only one possible plan.  Run it.  Build the stages from the solution.
            WorkingSet* ws = new WorkingSet();
            PlanStage* root;
            verify(StageBuilder::build(collection, *solutions[0], ws, &root));

            // And, run the plan.
            *out = new SingleSolutionRunner(collection,
                                            canonicalQuery.release(),
                                            solutions[0],
                                            root,
                                            ws);
            return Status::OK();
        }
        else {
            // Many solutions.  Create a MultiPlanStage to pick the best, update the cache, and so on.

            // The working set will be shared by all candidate plans and owned by the containing runner
            WorkingSet* sharedWorkingSet = new WorkingSet();

            MultiPlanStage* multiPlanStage = new MultiPlanStage(collection, rawCanonicalQuery);

            for (size_t ix = 0; ix < solutions.size(); ++ix) {
                if (solutions[ix]->cacheData.get()) {
                    solutions[ix]->cacheData->indexFilterApplied = plannerParams.indexFiltersApplied;
                }

                // version of StageBuild::build when WorkingSet is shared
                PlanStage* nextPlanRoot;
                verify(StageBuilder::build(collection, *solutions[ix],
                                           sharedWorkingSet, &nextPlanRoot));

                // Owns none of the arguments
                multiPlanStage->addPlan(solutions[ix], nextPlanRoot, sharedWorkingSet);
            }

            multiPlanStage->pickBestPlan();
            multiPlanStage->generateCandidateStats();

            *out = new SingleSolutionRunner(collection,
                                            canonicalQuery.release(),
                                            multiPlanStage->bestSolution(),
                                            multiPlanStage,
                                            sharedWorkingSet);

            return Status::OK();
        }
    }

    //
    // Count hack
    //

    namespace {

        /**
         * Returns 'true' if the provided solution 'soln' can be rewritten to use
         * a fast counting stage.  Mutates the tree in 'soln->root'.
         *
         * Otherwise, returns 'false'.
         */
        bool turnIxscanIntoCount(QuerySolution* soln) {
            QuerySolutionNode* root = soln->root.get();

            // Root should be a fetch w/o any filters.
            if (STAGE_FETCH != root->getType()) {
                return false;
            }

            if (NULL != root->filter.get()) {
                return false;
            }

            // Child should be an ixscan.
            if (STAGE_IXSCAN != root->children[0]->getType()) {
                return false;
            }

            IndexScanNode* isn = static_cast<IndexScanNode*>(root->children[0]);

            // No filters allowed and side-stepping isSimpleRange for now.  TODO: do we ever see
            // isSimpleRange here?  because we could well use it.  I just don't think we ever do see
            // it.

            if (NULL != isn->filter.get() || isn->bounds.isSimpleRange) {
                return false;
            }

            // Make sure the bounds are OK.
            BSONObj startKey;
            bool startKeyInclusive;
            BSONObj endKey;
            bool endKeyInclusive;

            if (!IndexBoundsBuilder::isSingleInterval( isn->bounds,
                                                       &startKey,
                                                       &startKeyInclusive,
                                                       &endKey,
                                                       &endKeyInclusive )) {
                return false;
            }

            // Make the count node that we replace the fetch + ixscan with.
            CountNode* cn = new CountNode();
            cn->indexKeyPattern = isn->indexKeyPattern;
            cn->startKey = startKey;
            cn->startKeyInclusive = startKeyInclusive;
            cn->endKey = endKey;
            cn->endKeyInclusive = endKeyInclusive;
            // Takes ownership of 'cn' and deletes the old root.
            soln->root.reset(cn);
            return true;
        }

        /**
         * Returns true if indices contains an index that can be
         * used with DistinctNode. Sets indexOut to the array index
         * of PlannerParams::indices.
         * Look for the index for the fewest fields.
         * Criteria for suitable index is that the index cannot be special
         * (geo, hashed, text, ...).
         *
         * Multikey indices are not suitable for DistinctNode when the projection
         * is on an array element. Arrays are flattened in a multikey index which
         * makes it impossible for the distinct scan stage (plan stage generated from
         * DistinctNode) to select the requested element by array index.
         *
         * Multikey indices cannot be used for the fast distinct hack if the field is dotted.
         * Currently the solution generated for the distinct hack includes a projection stage and
         * the projection stage cannot be covered with a dotted field.
         */
        bool getDistinctNodeIndex(const std::vector<IndexEntry>& indices,
                                  const std::string& field, size_t* indexOut) {
            invariant(indexOut);
            bool isDottedField = str::contains(field, '.');
            int minFields = std::numeric_limits<int>::max();
            for (size_t i = 0; i < indices.size(); ++i) {
                // Skip special indices.
                if (!IndexNames::findPluginName(indices[i].keyPattern).empty()) {
                    continue;
                }
                // Skip multikey indices if we are projecting on a dotted field.
                if (indices[i].multikey && isDottedField) {
                    continue;
                }
                int nFields = indices[i].keyPattern.nFields();
                // Pick the index with the lowest number of fields.
                if (nFields < minFields) {
                    minFields = nFields;
                    *indexOut = i;
                }
            }
            return minFields != std::numeric_limits<int>::max();
        }

        /**
         * Checks dotted field for a projection and truncates the
         * field name if we could be projecting on an array element.
         * Sets 'isIDOut' to true if the projection is on a sub document of _id.
         * For example, _id.a.2, _id.b.c.
         */
        std::string getProjectedDottedField(const std::string& field, bool* isIDOut) {
            // Check if field contains an array index.
            std::vector<std::string> res;
            mongo::splitStringDelim(field, &res, '.');

            // Since we could exit early from the loop,
            // we should check _id here and set '*isIDOut' accordingly.
            *isIDOut = ("_id" == res[0]);

            // Skip the first dotted component. If the field starts
            // with a number, the number cannot be an array index.
            int arrayIndex = 0;
            for (size_t i = 1; i < res.size(); ++i) {
                if (mongo::parseNumberFromStringWithBase(res[i], 10, &arrayIndex).isOK()) {
                    // Array indices cannot be negative numbers (this is not $slice).
                    // Negative numbers are allowed as field names.
                    if (arrayIndex >= 0) {
                        // Generate prefix of field up to (but not including) array index.
                        std::vector<std::string> prefixStrings(res);
                        prefixStrings.resize(i);
                        // Reset projectedField. Instead of overwriting, joinStringDelim() appends joined string
                        // to the end of projectedField.
                        std::string projectedField;
                        mongo::joinStringDelim(prefixStrings, &projectedField, '.');
                        return projectedField;
                    }
                }
            }

            return field;
        }

        /**
         * Creates a projection spec for a distinct command from the requested field.
         * In most cases, the projection spec will be {_id: 0, key: 1}.
         * The exceptions are:
         * 1) When the requested field is '_id', the projection spec will {_id: 1}.
         * 2) When the requested field could be an array element (eg. a.0),
         *    the projected field will be the prefix of the field up to the array element.
         *    For example, a.b.2 => {_id: 0, 'a.b': 1}
         *    Note that we can't use a $slice projection because the distinct command filters
         *    the results from the runner using the dotted field name. Using $slice will
         *    re-order the documents in the array in the results.
         */
        BSONObj getDistinctProjection(const std::string& field) {
            std::string projectedField(field);

            bool isID = false;
            if ("_id" == field) {
                isID = true;
            }
            else if (str::contains(field, '.')) {
                projectedField = getProjectedDottedField(field, &isID);
            }
            BSONObjBuilder bob;
            if (!isID) {
                bob.append("_id", 0);
            }
            bob.append(projectedField, 1);
            return bob.obj();
        }

    }  // namespace

    Status getRunnerCount(Collection* collection,
                          const BSONObj& query,
                          const BSONObj& hintObj,
                          Runner** out) {
        verify(collection);

        const WhereCallbackReal whereCallback(collection->ns().db());

        CanonicalQuery* cq;
        uassertStatusOK(CanonicalQuery::canonicalize(collection->ns().ns(),
                                                     query,
                                                     BSONObj(),
                                                     BSONObj(), 
                                                     0,
                                                     0,
                                                     hintObj,
                                                     &cq,
                                                     whereCallback));

        return getRunner(collection, cq, out, QueryPlannerParams::PRIVATE_IS_COUNT);
    }

    //
    // Distinct hack
    //

    Status getRunnerDistinct(Collection* collection,
                             const BSONObj& query,
                             const string& field,
                             Runner** out) {
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
                                                           desc->getAccessMethodName(),
                                                           desc->isMultikey(),
                                                           desc->isSparse(),
                                                           desc->indexName(),
                                                           desc->infoObj()));
            }
        }

        const WhereCallbackReal whereCallback(collection->ns().db());

        // If there are no suitable indices for the distinct hack bail out now into regular planning
        // with no projection.
        if (plannerParams.indices.empty()) {
            CanonicalQuery* cq;
            Status status = CanonicalQuery::canonicalize(
                                collection->ns().ns(), query, &cq, whereCallback);
            if (!status.isOK()) {
                return status;
            }

            // Takes ownership of cq.
            return getRunner(collection, cq, out);
        }

        //
        // If we're here, we have an index prefixed by the field we're distinct-ing over.
        //

        // Applying a projection allows the planner to try to give us covered plans that we can turn
        // into the projection hack.  getDistinctProjection deals with .find() projection semantics
        // (ie _id:1 being implied by default).
        BSONObj projection = getDistinctProjection(field);

        // Apply a projection of the key.  Empty BSONObj() is for the sort.
        CanonicalQuery* cq;
        Status status = CanonicalQuery::canonicalize(collection->ns().ns(),
                                                     query,
                                                     BSONObj(),
                                                     projection,
                                                     &cq,
                                                     whereCallback);
        if (!status.isOK()) {
            return status;
        }

        // If there's no query, we can just distinct-scan one of the indices.
        // Not every index in plannerParams.indices may be suitable. Refer to
        // getDistinctNodeIndex().
        size_t distinctNodeIndex = 0;
        if (query.isEmpty() &&
            getDistinctNodeIndex(plannerParams.indices, field, &distinctNodeIndex)) {
            DistinctNode* dn = new DistinctNode();
            dn->indexKeyPattern = plannerParams.indices[distinctNodeIndex].keyPattern;
            dn->direction = 1;
            IndexBoundsBuilder::allValuesBounds(dn->indexKeyPattern, &dn->bounds);
            dn->fieldNo = 0;

            QueryPlannerParams params;

            // Takes ownership of 'dn'.
            QuerySolution* soln = QueryPlannerAnalysis::analyzeDataAccess(*cq, params, dn);
            verify(soln);

            LOG(2) << "Using fast distinct: " << cq->toStringShort()
                   << ", planSummary: " << getPlanSummary(*soln);

            WorkingSet* ws = new WorkingSet();
            PlanStage* root;
            verify(StageBuilder::build(collection, *soln, ws, &root));
            *out = new SingleSolutionRunner(collection, cq, soln, root, ws);
            return Status::OK();
        }

        // See if we can answer the query in a fast-distinct compatible fashion.
        vector<QuerySolution*> solutions;
        status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        if (!status.isOK()) {
            return getRunner(collection, cq, out);
        }

        // We look for a solution that has an ixscan we can turn into a distinctixscan
        for (size_t i = 0; i < solutions.size(); ++i) {
            if (turnIxscanIntoDistinctIxscan(solutions[i], field)) {
                // Great, we can use solutions[i].  Clean up the other QuerySolution(s).
                for (size_t j = 0; j < solutions.size(); ++j) {
                    if (j != i) {
                        delete solutions[j];
                    }
                }

                LOG(2) << "Using fast distinct: " << cq->toStringShort()
                       << ", planSummary: " << getPlanSummary(*solutions[i]);

                // Build and return the SSR over solutions[i].
                WorkingSet* ws = new WorkingSet();
                PlanStage* root;
                verify(StageBuilder::build(collection, *solutions[i], ws, &root));
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

        // We drop the projection from the 'cq'.  Unfortunately this is not trivial.
        delete cq;
        status = CanonicalQuery::canonicalize(collection->ns().ns(), query, &cq, whereCallback);
        if (!status.isOK()) {
            return status;
        }

        // Takes ownership of cq.
        return getRunner(collection, cq, out);
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
