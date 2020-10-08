/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_explainer.h"

namespace mongo {

class Collection;
class CollectionPtr;
class OperationContext;
class PlanExecutorPipeline;
struct PlanSummaryStats;

/**
 * Namespace for the collection of static methods used to generate explain information.
 */
class Explain {
public:
    /**
     * Get explain BSON for the execution stages contained by 'exec'. Use this function if you
     * have a PlanExecutor and want to convert it into a human readable explain format. Any
     * operation which has a query component (e.g. find, update, group) can be explained via
     * this function.
     *
     * The explain information is extracted from 'exec' and added to the out-parameter 'out'.
     *
     * The explain information is generated with the level of detail specified by 'verbosity'.
     *
     * The 'extraInfo' parameter specifies additional information to include into the output.
     *
     * The 'command' parameter represents the command object that is being explained.
     *
     * Does not take ownership of its arguments.
     *
     * During this call it may be required to execute the plan to collect statistics. If the
     * PlanExecutor uses 'kLockExternally' lock policy, the caller should hold at least an IS lock
     * on the collection the that the query runs on, even if 'collection' parameter is nullptr.
     *
     * If there is an error during the execution of the query, the error message and code are
     * added to the "executionStats" section of the explain.
     */
    static void explainStages(PlanExecutor* exec,
                              const CollectionPtr& collection,
                              ExplainOptions::Verbosity verbosity,
                              BSONObj extraInfo,
                              const BSONObj& command,
                              BSONObjBuilder* out);
    /**
     * Adds "queryPlanner" and "executionStats" (if requested in verbosity) fields to 'out'. Unlike
     * the other overload of explainStages() above, this one does not add the "serverInfo" section.
     *
     * - 'exec' is the stage tree for the operation being explained.
     * - 'collection' is the relevant collection. During this call it may be required to execute the
     * plan to collect statistics. If the PlanExecutor uses 'kLockExternally' lock policy, the
     * caller should hold at least an IS lock on the collection the that the query runs on, even if
     * 'collection' parameter is nullptr.
     * - 'verbosity' is the verbosity level of the explain.
     * - 'extraInfo' specifies additional information to include into the output.
     * - 'executePlanStatus' is the status returned after executing the query (Status::OK if the
     * query wasn't executed).
     * - 'winningPlanTrialStats' is the stats of the winning plan during the trial period. May be
     * nullptr.
     * - 'command' represents the command object that is being explained.
     * - 'out' is the builder for the explain output.
     */
    static void explainStages(
        PlanExecutor* exec,
        const CollectionPtr& collection,
        ExplainOptions::Verbosity verbosity,
        Status executePlanStatus,
        boost::optional<PlanExplainer::PlanStatsDetails> winningPlanTrialStats,
        BSONObj extraInfo,
        const BSONObj& command,
        BSONObjBuilder* out);

    /**
     * Gets explain BSON for the document sources contained by 'exec'. Use this function if you have
     * a PlanExecutor for a pipeline and want to turn it into a human readable explain format.
     *
     * The explain information is generated with the level of detail specified by 'verbosity'.
     *
     * If 'verbosity' >= 'kExecStats' the 'executePipeline' flag is used to indicate whether the
     * pipeline needs to be executed first, before the stats is collected. Otherwise, it is assumed
     * that the plan was already executed until EOF and the stats are ready for collection.
     *
     * The 'command' parameter represents the command object that is being explained.
     */
    static void explainPipeline(PlanExecutor* exec,
                                bool executePipeline,
                                ExplainOptions::Verbosity verbosity,
                                const BSONObj& command,
                                BSONObjBuilder* out);

    /**
     * Serializes a PlanCacheEntry to the provided BSON object builder. The output format is
     * intended to be human readable, and useful for debugging query performance problems related to
     * the plan cache.
     */
    static void planCacheEntryToBSON(const PlanCacheEntry& entry, BSONObjBuilder* out);
};

}  // namespace mongo
