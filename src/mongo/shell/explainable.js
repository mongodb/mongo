//
// A view of a collection against which operations are explained rather than executed
// normally.
//

var Explainable = (function() {
    var parseVerbosity = function(verbosity) {
        // Truthy non-strings are interpreted as "allPlansExecution" verbosity.
        if (verbosity && (typeof verbosity !== "string")) {
            return "allPlansExecution";
        }

        // Falsy non-strings are interpreted as "queryPlanner" verbosity.
        if (!verbosity && (typeof verbosity !== "string")) {
            return "queryPlanner";
        }

        // All verbosity strings are passed through. Server validates if it is a known option.

        return verbosity;
    };

    var throwOrReturn = function(explainResult) {
        if (("ok" in explainResult && !explainResult.ok) || explainResult.$err) {
            throw _getErrorWithCode(explainResult, "explain failed: " + tojson(explainResult));
        }

        return explainResult;
    };

    var buildExplainCmd = function(innerCmd, verbosity) {
        var explainCmd = {"explain": innerCmd, "verbosity": verbosity};
        // If "maxTimeMS" is set on innerCmd, it needs to be propagated to the top-level
        // of explainCmd so that it has the intended effect.
        if (innerCmd.hasOwnProperty("maxTimeMS")) {
            explainCmd.maxTimeMS = innerCmd.maxTimeMS;
        }
        return explainCmd;
    };

    function constructor(collection, verbosity) {
        //
        // Private vars.
        //

        this._collection = collection;
        this._verbosity = parseVerbosity(verbosity);

        //
        // Public methods.
        //

        this.getCollection = function() {
            return this._collection;
        };

        this.getVerbosity = function() {
            return this._verbosity;
        };

        this.setVerbosity = function(verbosity) {
            this._verbosity = parseVerbosity(verbosity);
            return this;
        };

        this.help = function() {
            print("Explainable operations");
            print("\t.aggregate(...) - explain an aggregation operation");
            print("\t.count(...) - explain a count operation");
            print("\t.distinct(...) - explain a distinct operation");
            print("\t.find(...) - get an explainable query");
            print("\t.findAndModify(...) - explain a findAndModify operation");
            print("\t.mapReduce(...) - explain a mapReduce operation");
            print("\t.remove(...) - explain a remove operation");
            print("\t.update(...) - explain an update operation");
            print("Explainable collection methods");
            print("\t.getCollection()");
            print("\t.getVerbosity()");
            print("\t.setVerbosity(verbosity)");
            return __magicNoPrint;
        };

        //
        // Pretty representations.
        //

        this.toString = function() {
            return "Explainable(" + this._collection.getFullName() + ")";
        };

        this.shellPrint = function() {
            return this.toString();
        };

        //
        // Explainable operations.
        //

        this.aggregate = function(pipeline, extraOpts) {
            if (!(pipeline instanceof Array)) {
                // Support legacy varargs form. (Also handles db.foo.aggregate())
                pipeline = Array.from(arguments);
                extraOpts = {};
            }

            // Add the explain option.
            let extraOptsCopy = Object.extend({}, (extraOpts || {}));

            // For compatibility with 3.4 and older versions, when the verbosity is "queryPlanner",
            // we use the explain option to the aggregate command. Otherwise we issue an explain
            // command wrapping the agg command, which is supported by newer versions of the server.
            if (this._verbosity === "queryPlanner") {
                extraOptsCopy.explain = true;
                return this._collection.aggregate(pipeline, extraOptsCopy);
            } else {
                // The aggregate command requires a cursor field.
                if (!extraOptsCopy.hasOwnProperty("cursor")) {
                    extraOptsCopy = Object.extend(extraOptsCopy, {cursor: {}});
                }

                let aggCmd = Object.extend(
                    {"aggregate": this._collection.getName(), "pipeline": pipeline}, extraOptsCopy);
                let explainCmd = buildExplainCmd(aggCmd, this._verbosity);
                let explainResult = this._collection.runReadCommand(explainCmd);
                return throwOrReturn(explainResult);
            }
        };

        this.count = function(query, options) {
            query = this.find(query);
            return QueryHelpers._applyCountOptions(query, options).count();
        };

        /**
         * .explain().find() and .find().explain() mean the same thing. In both cases, we use
         * the DBExplainQuery abstraction in order to construct the proper explain command to send
         * to the server.
         */
        this.find = function() {
            var cursor = this._collection.find.apply(this._collection, arguments);
            return new DBExplainQuery(cursor, this._verbosity);
        };

        this.findAndModify = function(params) {
            var famCmd = Object.extend({"findAndModify": this._collection.getName()}, params);
            var explainCmd = buildExplainCmd(famCmd, this._verbosity);
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.distinct = function(keyString, query, options) {
            var distinctCmd = {
                distinct: this._collection.getName(),
                key: keyString,
                query: query || {}
            };

            if (options && options.hasOwnProperty("collation")) {
                distinctCmd.collation = options.collation;
            }
            if (options && options.hasOwnProperty("maxTimeMS")) {
                distinctCmd.maxTimeMS = options.maxTimeMS;
            }

            var explainCmd = buildExplainCmd(distinctCmd, this._verbosity);
            var explainResult = this._collection.runReadCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.remove = function() {
            var parsed = this._collection._parseRemove.apply(this._collection, arguments);
            var query = parsed.query;
            var justOne = parsed.justOne;
            var collation = parsed.collation;

            var bulk = this._collection.initializeOrderedBulkOp();
            var removeOp = bulk.find(query);

            if (collation) {
                removeOp.collation(collation);
            }

            if (justOne) {
                removeOp.removeOne();
            } else {
                removeOp.remove();
            }

            var explainCmd = bulk.convertToExplainCmd(this._verbosity);
            var explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.update = function() {
            var parsed = this._collection._parseUpdate.apply(this._collection, arguments);
            var query = parsed.query;
            var updateSpec = parsed.updateSpec;
            var upsert = parsed.upsert;
            var multi = parsed.multi;
            var collation = parsed.collation;
            var arrayFilters = parsed.arrayFilters;
            var hint = parsed.hint;

            var bulk = this._collection.initializeOrderedBulkOp();
            var updateOp = bulk.find(query);

            if (hint) {
                updateOp.hint(hint);
            }

            if (upsert) {
                updateOp = updateOp.upsert();
            }

            if (collation) {
                updateOp.collation(collation);
            }

            if (arrayFilters) {
                updateOp.arrayFilters(arrayFilters);
            }

            if (multi) {
                updateOp.update(updateSpec);
            } else {
                updateOp.updateOne(updateSpec);
            }

            var explainCmd = bulk.convertToExplainCmd(this._verbosity);
            var explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };

        this.mapReduce = function(map, reduce, optionsObjOrOutString) {
            assert(optionsObjOrOutString, "Must supply the 'optionsObjOrOutString ' argument");

            const mapReduceCmd = {mapreduce: this._collection.getName(), map: map, reduce: reduce};

            if (typeof (optionsObjOrOutString) == "string")
                mapReduceCmd["out"] = optionsObjOrOutString;
            else
                Object.extend(mapReduceCmd, optionsObjOrOutString);

            const explainCmd = buildExplainCmd(mapReduceCmd, this._verbosity);
            const explainResult = this._collection.runCommand(explainCmd);
            return throwOrReturn(explainResult);
        };
    }

    //
    // Public static methods.
    //

    constructor.parseVerbosity = parseVerbosity;
    constructor.throwOrReturn = throwOrReturn;

    return constructor;
})();

/**
 * Provides a namespace for explain-related helper functions.
 */
let Explain = (function() {
    /**
     * Given explain output from the server for a query that used the slot-based execution engine
     * (SBE), presents this information in a more consumable format. This function requires that the
     * explain output comes from a system where SBE is enabled on all nodes, and wil throw an
     * exception if it observes explain output from the classic execution engine.
     *
     * Rather than an object, this function returns an array of information about each candidate
     * plan. The first element of the array is always the winning plan. Each of the plans is
     * represented by an object with the following schema:
     *
     *  {
     *    planNumber: <number>,
     *    queryPlan: <obj>,
     *    summaryStats: <obj>,
     *    trialSummaryStats: <obj>,
     *    slotBasedPlan: <string>,
     *    slotBasedPlanVerbose: <obj>,
     *    trialSlotBasedPlanVerbose: <obj>,
     *  }
     *
     * The meanings of the returned fields:
     *   - 'planNumber': An integer giving the index of the plan in the array. The winning plan is
     *   0, the first candidate is 1, and so on. Allows plans to be identified easily.
     *   - 'queryPlan' - A description of the query plan, using a representation which should be
     *   familiar to consumers of explains from versions before SBE. Importantly, each node in the
     *   plan tree may be augmented with the following:
     *     * A field called "execStats". These are derived execution stats for the node, obtained
     *     based on the full SBE exec stats output. Only present for the winning plan.
     *     * A field called "trialExecStats". This is identical in meaning to "execStats", but shows
     *     stats from the multi-planner's trial execution period. These stats are derived from the
     *     'allPlansExecution' section.
     *   - 'summaryStats' - An object containing high-level execution stats for the plan as a whole
     *     (e.g. "totalKeysExamined" and "totalDocsExamined"). Only present for the winning plan.
     *   - 'trialSummaryStats' - Similar to "summaryStats", but describes execution stats from the
     *   multi-planner's trial execution period. These stats are derived from the
     *   'allPlansExecution' section.
     *   - 'slotBasedPlan' - A string giving a concise representation of the SBE plan.
     *   - 'slotBasedPlanVerbose' - The full SBE plan with execution stats associated with every
     *   node. only present for the winning plan.
     *   - 'trialSlotBasedPlanVerbose' - The SBE plans reported in the 'allPlansExecution' section,
     *      whose runtime stats describe what happened during the multi-planner's trial execution
     *      period.
     *
     * This function will work for find-style explain output as well as aggregate explain output. In
     * the case of agg, it produces output in the same format by reformatting the contents of the
     * $cursor stage.
     *
     * It is also designed to work for explain output from sharded find commands and sharded agg
     * commands. In such cases, the output is an object with one key per shard:
     *
     *  {
     *    <shardName1>: [...],
     *    <shardName2>: [...],
     *    ...
     *  }
     *
     * Each shard reports information about its candidate plans in the same format as desgined
     * above.
     *
     * The second argument, 'fieldsToKeep', is an array of field names from the above schema. For
     * instance, a common use case would be to just see 'planNumber' and 'queryPlan', in which case
     * the caller should pass a vlaue of ["planNumber", "queryPlan"]. This argument is optional, and
     * defaults to generating the full verbose output.
     *
     * This may mutate 'explain'. Callers which want to leave 'explain' unmodified should create a
     * deep copy prior to calling this function.
     */
    function sbeReformatExperimental(explain, fieldsToKeep) {
        const kCommonStats = [
            "nReturned",
            "opens",
            "closes",
            "saveState",
            "restoreState",
            "isEOF",
            "executionTimeMillisEstimate",
        ];

        const kSummaryStats = [
            "executionSuccess",
            "nReturned",
            "executionTimeMillis",
            "totalKeysExamined",
            "totalDocsExamined",
        ];

        const kTrialSummaryStats = [
            "nReturned",
            "executionTimeMillisEstimate",
            "totalKeysExamined",
            "totalDocsExamined",
        ];

        const kAggregateStats = [
            "seeks",
            "numReads",
        ];

        // Given either a query solution or an SBE tree represented by 'root', walks the tree and
        // calls 'callbackFn()' for each node. This is done in a bottom-up manner so that the
        // callback is executed last on the root.
        function walkTree(root, callbackFn) {
            if ("inputStages" in root) {
                for (var i = 0; i < root.inputStages.length; i++) {
                    walkTree(root.inputStages[i], callbackFn);
                }
            }

            for (let childStageName
                     of ["inputStage", "thenStage", "elseStage", "outerStage", "innerStage"]) {
                if (childStageName in root) {
                    walkTree(root[childStageName], callbackFn);
                }
            }

            callbackFn(root);
        }

        function aggregateStat(target, statObjName, propName, init, value, aggFunc) {
            if (!target.hasOwnProperty(statObjName)) {
                target[statObjName] = {};
            }

            const statObj = target[statObjName];
            if (!statObj.hasOwnProperty(propName)) {
                statObj[propName] = init;
            }
            statObj[propName] = aggFunc(statObj[propName], value);
        }

        function takeLatest(target, statObjName, propName, value) {
            aggregateStat(target, statObjName, propName, 0, value, (oldVal, newVal) => newVal);
        }

        function sumIfPresent(target, statObjName, propName, value) {
            if (!value) {
                return;
            }

            aggregateStat(
                target, statObjName, propName, 0, value, (oldVal, newVal) => oldVal + newVal);
        }

        function fillAggregatedExecStats(sbeExecStats, querySolution, statObjName) {
            const executionStages = sbeExecStats.executionStages;

            // Construct a map from "planNodeId" to the object representing that plan node in the
            // serialization of the query solution.
            let nodeIdToNode = {};
            walkTree(querySolution, (node) => {
                nodeIdToNode[node.planNodeId] = node;
            });

            // This time walk the SBE plan and attribute stats to the corresponding node in the
            // query solution tree.
            walkTree(executionStages, (node) => {
                // Find the QSN to which we should attribute this SBE stage's runtime stats.
                const correspondingNode = nodeIdToNode[node.planNodeId];
                assert.neq(correspondingNode, null);
                assert.neq(correspondingNode, undefined);

                // We generally only care about the common stats of the subtree as a whole
                // associated with a query solution node, so use 'takeLatest()' to report the stats
                // associated with the root node of this subtree.
                for (let stat of kCommonStats) {
                    takeLatest(correspondingNode, statObjName, stat, node[stat]);
                }

                for (let stat of kAggregateStats) {
                    sumIfPresent(correspondingNode, statObjName, stat, node[stat]);
                }
            });
        }

        function addSummaryStats(executionStats, summaryStatsName, statsToAdd, target) {
            target[summaryStatsName] = {};
            let summaryStats = target[summaryStatsName];

            for (let stat of statsToAdd) {
                summaryStats[stat] = executionStats[stat];
            }
        }

        function handleExecutionStats(executionStats, reformatted) {
            // First, specially handle the full execution stats associated with the winning plan.
            assert(typeof executionStats === "object");
            assert(executionStats.hasOwnProperty("executionStages"));
            reformatted[0].slotBasedPlanVerbose = executionStats.executionStages;
            fillAggregatedExecStats(executionStats, reformatted[0].queryPlan, "execStats");
            addSummaryStats(executionStats, "summaryStats", kSummaryStats, reformatted[0]);

            const allPlansExecution = executionStats.allPlansExecution;
            if (!allPlansExecution) {
                return;
            }

            if (!Array.isArray(allPlansExecution)) {
                throw Error("expected 'allPlansExecution' section to be an array");
            }

            if (allPlansExecution.length === 0) {
                // This can happen if the user requested 'allPlansExecution' verbosity, but the
                // query only had one solution (so there was no runtime planning).
                return;
            }

            assert.eq(reformatted.length, allPlansExecution.length);
            for (let i = 0; i < reformatted.length; ++i) {
                assert(allPlansExecution[i].hasOwnProperty("executionStages"));
                reformatted[i].trialSlotBasedPlanVerbose = allPlansExecution[i].executionStages;
                fillAggregatedExecStats(
                    allPlansExecution[i], reformatted[i].queryPlan, "trialExecStats");
                addSummaryStats(
                    allPlansExecution[i], "trialSummaryStats", kTrialSummaryStats, reformatted[i]);
            }
        }

        function project(reformatted, fieldsToKeep) {
            for (let plan of reformatted) {
                for (let field of Object.keys(plan)) {
                    if (!fieldsToKeep.has(field)) {
                        delete plan[field];
                    }
                }
            }
        }

        function doReformatting(queryPlanner, executionStats, fieldsToKeep) {
            assert(queryPlanner.hasOwnProperty("winningPlan"));
            assert(queryPlanner.hasOwnProperty("rejectedPlans"));
            const winningPlan = queryPlanner.winningPlan;
            const rejectedPlans = queryPlanner.rejectedPlans;

            if (!winningPlan.hasOwnProperty("slotBasedPlan")) {
                // Part of this query did not use SBE.
                throw Error("Found winning plan which did not use SBE");
            }

            // We will assign a number to each candidate plan, in order to make referring the plans
            // easier.
            let planNumber = 0;

            const reformatted = [];

            winningPlan.planNumber = planNumber++;
            reformatted.push(winningPlan);

            for (let plan of rejectedPlans) {
                if (!plan.hasOwnProperty("slotBasedPlan")) {
                    throw Error("Found rejected plan which did not use SBE, planNumber: " +
                                planNumber);
                }

                plan.planNumber = planNumber++;
                reformatted.push(plan);
            }

            // Only massage the execution stats for this particular piece of the explain output if
            // this node used the slot-based execution engine for this query. The reformatting done
            // here is tied to the SBE explain format.
            if (executionStats) {
                handleExecutionStats(executionStats, reformatted);
            }

            if (fieldsToKeep) {
                project(reformatted, fieldsToKeep);
            }

            return reformatted;
        }

        function doShardedFindReformatting(fullExplain, fieldsToKeep) {
            assert(fullExplain.hasOwnProperty("queryPlanner"));
            const queryPlanner = fullExplain.queryPlanner;

            // Return an object where the keys are the names of the shards, and the values is a
            // reformatted array similar to what would be returned for an explain from a standalone
            // or replica set node.
            const result = {};

            assert(queryPlanner.hasOwnProperty("winningPlan"));
            const winningPlan = queryPlanner.winningPlan;
            assert(winningPlan.hasOwnProperty("shards"));
            const shardsQueryPlanner = winningPlan.shards;

            for (let shard of shardsQueryPlanner) {
                assert(shard.hasOwnProperty("shardName"));
                assert(shard.hasOwnProperty("winningPlan"));
                assert(!shard.hasOwnProperty("executionStats"));

                const shardName = shard.shardName;

                // We're just handling the "queryPlanner" section here, so we don't pass execution
                // stats information.
                result[shardName] = doReformatting(shard, undefined, fieldsToKeep);
            }

            const executionStats = fullExplain.executionStats;
            if (!executionStats) {
                return result;
            }

            // Next handle "executionStats", which is an entirely separate section from
            // "queryPlanner" and shows execution-level information gathered from each shard.
            assert(executionStats.hasOwnProperty("executionStages"));
            const executionStages = executionStats.executionStages;
            assert(executionStages.hasOwnProperty("shards"));
            const shardsExecStats = executionStages.shards;

            for (let shard of shardsExecStats) {
                assert(shard.hasOwnProperty("shardName"));
                const shardName = shard.shardName;
                assert(result.hasOwnProperty(shardName));

                const shardResults = result[shardName];
                handleExecutionStats(shard, shardResults);

                // We may add more fields here, so re-apply the projection.
                if (fieldsToKeep) {
                    project(shardResults, fieldsToKeep);
                }
            }

            return result;
        }

        function doFindReformatting(fullExplain, fieldsToKeep) {
            assert(fullExplain.hasOwnProperty("queryPlanner"));
            const queryPlanner = fullExplain.queryPlanner;

            // The "executionStats" field may or may not be present.
            const executionStats = fullExplain.executionStats;

            return doReformatting(queryPlanner, executionStats, fieldsToKeep);
        }

        function doAggReformatting(fullExplain, fieldsToKeep) {
            // We are explaining an aggregate operation. Look for the $cursor stage and report
            // information about that.
            const stages = fullExplain.stages;
            assert.gt(stages.length, 0, stages);
            const firstStage = stages[0];

            if (!firstStage.hasOwnProperty("$cursor")) {
                throw Error("could not find $cursor stage");
            }

            // The contents of the $cursor stage should be similarly formatted to an explain of a
            // find operation, so reformat these inner contents.
            const cursorStage = firstStage.$cursor;
            return doFindReformatting(cursorStage, fieldsToKeep);
        }

        function doShardedAggReformatting(fullExplain, fieldsToKeep) {
            const result = {};

            // Sharded agg explain result reports the full explains from each of the shards. All we
            // need to do is reformat the explains from the shards one-by-one.
            for (let shardName of Object.keys(fullExplain.shards)) {
                const shardExplain = fullExplain.shards[shardName];
                result[shardName] = doUnshardedReformatting(shardExplain, fieldsToKeep);
            }

            return result;
        }

        /**
         * Performs reformatting for an explain from a single mongod. Infers whether the explain is
         * a find explain or an aggregate explain.
         */
        function doUnshardedReformatting(fullExplain, fieldsToKeep) {
            // The presence of a top-level "stages" array identifies that this is aggregation
            // explain output from a mongod.
            if (fullExplain.hasOwnProperty("stages")) {
                return doAggReformatting(fullExplain, fieldsToKeep);
            }

            // If we are not in any of the other special cases, then assume that this is a find
            // explain from a mongod.
            return doFindReformatting(fullExplain, fieldsToKeep);
        }

        if (fieldsToKeep && !Array.isArray(fieldsToKeep)) {
            throw Error("second argument must be undefined or an array");
        }
        const fieldsToKeepSet = fieldsToKeep ? new Set(fieldsToKeep) : undefined;

        // Bail out if the explain version indicates that the classic engine was used.
        if (explain.explainVersion && explain.explainVersion !== "2") {
            throw Error("'explainVersion' is " + explain.explainVersion + " but expected '2'");
        }

        // Infer what kind of explain we're dealing with. Only sharded agg results will report a
        // "shards" array at the top-level.
        if (explain.hasOwnProperty("shards")) {
            return doShardedAggReformatting(explain, fieldsToKeepSet);
        }

        const queryPlanner = explain.queryPlanner;
        const isShardedFind = queryPlanner && queryPlanner.mongosPlannerVersion === 1;
        if (isShardedFind) {
            return doShardedFindReformatting(explain, fieldsToKeepSet);
        }

        // We've handled the sharded cases already, so we now assume that the given explain output
        // is from a single mongod.
        return doUnshardedReformatting(explain, fieldsToKeepSet);
    }

    function help() {
        print(
            `
Explain.sbeReformatExperimental(<explain>, <fieldsToKeep>)

\tExperimental helper which takes explain output from a system where the slot-based
\tengine (SBE) is enabled and presents it in a more consumable fashion. Returns an
\tarray of plans where each plan has the following schema:

\t  {
\t    planNumber: <number>,
\t    queryPlan: <obj>,
\t    summaryStats: <obj>,
\t    trialSummaryStats: <obj>,
\t    slotBasedPlan: <string>,
\t    slotBasedPlanVerbose: <obj>,
\t    trialSlotBasedPlanVerbose: <obj>,
\t  }

\t<explain> - output from an explain operation
\t<fieldsToKeep> - an optional array of field names to include for each candidate plan
`);
        return __magicNoPrint;
    }

    return {
        help: help,
        sbeReformatExperimental: sbeReformatExperimental,
    };
})();

/**
 * This is the user-facing method for creating an Explainable from a collection.
 */
DBCollection.prototype.explain = function(verbosity) {
    return new Explainable(this, verbosity);
};
