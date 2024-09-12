import {
    everyWinningPlan,
    flattenQueryPlanTree,
    getAggPlanStages,
    getEngine,
    getPlanStages,
    getQueryPlanners,
    getWinningPlan,
    isAlwaysFalsePlan,
    isEofPlan,
    isIdhackOrExpress,
    planHasStage,
} from "jstests/libs/analyze_plan.js";
import {getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {
    checkSbeFullFeatureFlagEnabled,
    checkSbeRestrictedOrFullyEnabled
} from "jstests/libs/sbe_util.js";

/**
 * Class containing common test functions used in query_settings_index_application_* tests.
 */
export class QuerySettingsIndexHintsTests {
    /**
     * Create a query settings utility class.
     */
    constructor(qsutils) {
        this._qsutils = qsutils;
        this._db = qsutils._db;
        this.indexA = {a: 1};
        this.indexB = {b: 1};
        this.indexAB = {a: 1, b: 1};
    }

    /**
     * Asserts that after executing 'command' the most recent query plan from cache would have
     * 'querySettings' set.
     */
    assertQuerySettingsInCacheForCommand(command,
                                         querySettings,
                                         collOrViewName = this._qsutils._collName) {
        const explainCmd = getExplainCommand(command);
        const explain = assert.commandWorked(
            this._db.runCommand(explainCmd),
            `Failed running explain command ${
                tojson(explainCmd)} for checking the query settings plan cache check.`);
        const isIdhackQuery =
            everyWinningPlan(explain, (winningPlan) => isIdhackOrExpress(this._db, winningPlan));
        const isMinMaxQuery = "min" in command || "max" in command;
        const isTriviallyFalse = everyWinningPlan(
            explain,
            (winningPlan) => isEofPlan(this._db, winningPlan) || isAlwaysFalsePlan(winningPlan));
        const {defaultReadPreference, defaultReadConcernLevel, networkErrorAndTxnOverrideConfig} =
            TestData;
        const performsSecondaryReads =
            defaultReadPreference && defaultReadPreference.mode == "secondary";
        const isInTxnPassthrough = networkErrorAndTxnOverrideConfig &&
            networkErrorAndTxnOverrideConfig.wrapCRUDinTransactions;
        const willRetryOnNetworkErrors = networkErrorAndTxnOverrideConfig &&
            networkErrorAndTxnOverrideConfig.retryOnNetworkErrors;

        // If the collection used is a view, determine the underlying collection being used.
        const collInfo = this._db.getCollectionInfos({name: collOrViewName})[0];
        let collName = collOrViewName;
        if (collInfo !== undefined && collInfo.options.hasOwnProperty('viewOn')) {
            collName = collInfo.options.viewOn;
        }
        const collHasPartialIndexes = this._db[collName].getIndexes().some(
            (idx) => idx.hasOwnProperty('partialFilterExpression'));

        const shouldCheckPlanCache =
            // Single solution plans are not cached in classic, therefore do not perform plan cache
            // checks for when the classic cache is used. Note that the classic cache is used
            // by default for SBE, except when featureFlagSbeFull is on.
            // TODO SERVER-90880: We can relax this check when we cache single-solution plans in the
            // classic cache with SBE.
            // TODO SERVER-13341: Relax this check to include the case where classic is being used.
            // TODO SERVER-94392: Relax this check when SBE plan cache supports partial indexes.
            (getEngine(explain) === "sbe" && checkSbeFullFeatureFlagEnabled(this._db) &&
             !collHasPartialIndexes) &&
            // Express or IDHACK optimized queries are not cached.
            !isIdhackQuery &&
            // Min/max queries are not cached.
            !isMinMaxQuery &&
            // Similarly, trivially false plans are not cached.
            !isTriviallyFalse &&
            // Subplans are cached differently from normal plans.
            !planHasStage(this._db, explain, "OR") &&
            // If query is executed on secondaries, do not assert the cache.
            !performsSecondaryReads &&
            // Do not check plan cache if causal consistency is enabled.
            !this._db.getMongo().isCausalConsistency() &&
            // $planCacheStats can not be run in transactions.
            !isInTxnPassthrough &&
            // Retrying on network errors most likely is related to stepdown, which does not go
            // together with plan cache clear.
            !willRetryOnNetworkErrors &&
            // If read concern is explicitly set, avoid plan cache checks.
            !defaultReadConcernLevel &&
            // If the test is performing initial sync it may affect the plan cache by generating an
            // additional entry.
            !TestData.isRunningInitialSync &&
            // If the test is running shard key analysis, it may affect the plan cache by generating
            // and additional entry.
            !TestData.isAnalyzingShardKey;

        if (!shouldCheckPlanCache) {
            return;
        }

        // Clear the plan cache before running any queries.
        this._db[collName].getPlanCache().clear();

        // Take the plan cache entries and ensure that they contain the 'settings'.
        assert.commandWorked(this._db.runCommand(command),
                             `Failed to check the plan cache because the original command failed ${
                                 tojson(command)}`);
        const planCacheStatsAfterRunningCmd = this._db[collName].getPlanCache().list();
        assert.gte(planCacheStatsAfterRunningCmd.length,
                   1,
                   "Expecting at least 1 entry in query plan cache");
        planCacheStatsAfterRunningCmd.forEach(
            plan => assert.docEq(this._qsutils.wrapIndexHintsIntoArrayIfNeeded(querySettings),
                                 plan.querySettings,
                                 plan));
    }

    assertIndexUse(cmd, expectedIndex, stagesExtractor, expectedStrategy) {
        const explain = assert.commandWorked(this._db.runCommand({explain: cmd}));
        const stagesUsingIndex = stagesExtractor(explain);
        if (expectedIndex !== undefined) {
            assert.gte(stagesUsingIndex.length, 1, explain);
        }
        for (const stage of stagesUsingIndex) {
            if (expectedIndex !== undefined) {
                assert.docEq(stage.keyPattern, expectedIndex, explain);
            }

            if (expectedStrategy !== undefined) {
                assert.docEq(stage.strategy, expectedStrategy, explain);
            }
        }
    }

    assertIndexScanStage(cmd, expectedIndex, ns) {
        return this.assertIndexUse(cmd, expectedIndex, (explain) => {
            return getQueryPlanners(explain)
                .filter(queryPlanner => queryPlanner.namespace == `${ns.db}.${ns.coll}`)
                .map(getWinningPlan)
                .flatMap(winningPlan => getPlanStages(winningPlan, "IXSCAN"));
        });
    }

    assertLookupJoinStage(cmd, expectedIndex, isSecondaryCollAView, expectedStrategy) {
        // $lookup stage is only pushed down to find in SBE and not in classic and only for
        // collections (not views).
        const expectPushDown = checkSbeRestrictedOrFullyEnabled(this._db) && !isSecondaryCollAView;
        if (!expectPushDown && expectedIndex != undefined) {
            return this.assertLookupPipelineStage(cmd, expectedIndex);
        }

        this.assertIndexUse(cmd, expectedIndex, (explain) => {
            return getQueryPlanners(explain)
                .map(getWinningPlan)
                .flatMap(winningPlan => getPlanStages(winningPlan, "EQ_LOOKUP"))
                .map(stage => {
                    stage.keyPattern = stage.indexKeyPattern;
                    return stage;
                });
        }, expectedStrategy);
    }

    assertLookupPipelineStage(cmd, expectedIndex) {
        const indexToKeyPatternMap = {"a_1": {a: 1}, "b_1": {b: 1}, "a_1_b_1": {a: 1, b: 1}};
        return this.assertIndexUse(cmd, expectedIndex, (explain) => {
            return getAggPlanStages(explain, "$lookup").map(stage => {
                let {indexesUsed, ...stageData} = stage;
                assert.eq(indexesUsed.length, 1, stage);
                stageData.keyPattern = indexToKeyPatternMap[indexesUsed[0]];
                return stageData;
            });
        });
    }

    assertDistinctScanStage(cmd, expectedIndex) {
        return this.assertIndexUse(cmd, expectedIndex, (explain) => {
            return getQueryPlanners(explain)
                .map(getWinningPlan)
                .flatMap(winningPlan => getPlanStages(winningPlan, "DISTINCT_SCAN"));
        });
    }

    assertCollScanStage(cmd, allowedDirections) {
        const explain = assert.commandWorked(this._db.runCommand({explain: cmd}));
        const collscanStages = getQueryPlanners(explain)
                                   .map(getWinningPlan)
                                   .flatMap(winningPlan => getPlanStages(winningPlan, "COLLSCAN"));
        assert.gte(collscanStages.length, 1, explain);
        for (const collscanStage of collscanStages) {
            assert(allowedDirections.includes(collscanStage.direction), explain);
        }
    }

    /**
     * Ensure query settings are applied as expected in a straightforward scenario.
     */
    assertQuerySettingsIndexApplication(querySettingsQuery, ns) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexB, this.indexAB]) {
            const settings = {indexHints: {ns, allowedIndexes: [index]}};
            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, index, ns);
                this.assertQuerySettingsInCacheForCommand(query, settings, ns.coll);
            });
        }
    }

    /**
     * Ensure query plan cache contains query settings for the namespace 'ns'.
     */
    assertGraphLookupQuerySettingsInCache(querySettingsQuery, ns) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const allowedIndexes of [[this.indexA, this.indexB],
                                      [this.indexA, this.indexAB],
                                      [this.indexAB, this.indexB]]) {
            const settings = {indexHints: {ns, allowedIndexes}};
            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertQuerySettingsInCacheForCommand(query, settings, ns.coll);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of the equi-join over namespace 'ns'.
     */
    assertQuerySettingsLookupJoinIndexApplication(querySettingsQuery, ns, isSecondaryCollAView) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexAB]) {
            const settings = {indexHints: {ns, allowedIndexes: [index]}};
            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertLookupJoinStage(query, index, isSecondaryCollAView);
                this.assertQuerySettingsInCacheForCommand(query, settings);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of $lookup equi-join for both collections.
     */
    assertQuerySettingsIndexAndLookupJoinApplications(querySettingsQuery,
                                                      mainNs,
                                                      secondaryNs,
                                                      isSecondaryCollAView) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const [mainCollIndex, secondaryCollIndex] of selfCrossProduct(
                 [this.indexA, this.indexAB])) {
            const settings = {
                indexHints: [
                    {ns: mainNs, allowedIndexes: [mainCollIndex]},
                    {ns: secondaryNs, allowedIndexes: [secondaryCollIndex]},
                ]
            };

            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, mainCollIndex, mainNs);
                this.assertLookupJoinStage(query, secondaryCollIndex, isSecondaryCollAView);
                this.assertQuerySettingsInCacheForCommand(query, settings, mainNs.coll);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of $lookup sub-pipeline.
     */
    assertQuerySettingsLookupPipelineIndexApplication(querySettingsQuery, ns) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexB, this.indexAB]) {
            const settings = {indexHints: {ns, allowedIndexes: [index]}};
            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertLookupPipelineStage(query, index);
                this.assertQuerySettingsInCacheForCommand(query, settings);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of $lookup sub-pipeline for both
     * collections.
     */
    assertQuerySettingsIndexAndLookupPipelineApplications(querySettingsQuery, mainNs, secondaryNs) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const [mainCollIndex, secondaryCollIndex] of selfCrossProduct(
                 [this.indexA, this.indexB, this.indexAB])) {
            const settings = {
                indexHints: [
                    {ns: mainNs, allowedIndexes: [mainCollIndex]},
                    {ns: secondaryNs, allowedIndexes: [secondaryCollIndex]},
                ]
            };

            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, mainCollIndex, mainNs);
                this.assertLookupPipelineStage(query, secondaryCollIndex);
                this.assertQuerySettingsInCacheForCommand(query, settings, mainNs.coll);
                this.assertQuerySettingsInCacheForCommand(query, settings, secondaryNs.coll);
            });
        }
    }

    /**
     * Ensure query settings are applied for both collections, resulting in index scans using the
     * hinted indexes.
     */
    assertQuerySettingsIndexApplications(querySettingsQuery, mainNs, secondaryNs) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        for (const [mainCollIndex, secondaryCollIndex] of selfCrossProduct(
                 [this.indexA, this.indexB, this.indexAB])) {
            const settings = {
                indexHints: [
                    {ns: mainNs, allowedIndexes: [mainCollIndex]},
                    {ns: secondaryNs, allowedIndexes: [secondaryCollIndex]},
                ]
            };

            this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, mainCollIndex, mainNs);
                this.assertIndexScanStage(query, secondaryCollIndex, secondaryNs);
                this.assertQuerySettingsInCacheForCommand(query, settings, mainNs.coll);
                this.assertQuerySettingsInCacheForCommand(query, settings, secondaryNs.coll);
            });
        }
    }

    /**
     * Ensure query settings '$natural' hints are applied as expected in a straightforward scenario.
     * This test case covers the following scenarios:
     * - Only forward scans allowed.
     * - Only backward scans allowed.
     * - Both forward and backward scans allowed.
     */
    assertQuerySettingsNaturalApplication(querySettingsQuery,
                                          ns,
                                          additionalHints = [],
                                          additionalAssertions = () => {}) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        const naturalForwardScan = {$natural: 1};
        const naturalForwardSettings = {
            indexHints: [{ns, allowedIndexes: [naturalForwardScan]}, ...additionalHints]
        };
        this._qsutils.withQuerySettings(querySettingsQuery, naturalForwardSettings, () => {
            this.assertCollScanStage(query, ["forward"]);
            this.assertQuerySettingsInCacheForCommand(query, naturalForwardSettings);
            additionalAssertions();
        });

        const naturalBackwardScan = {$natural: -1};
        const naturalBackwardSettings = {
            indexHints: [{ns, allowedIndexes: [naturalBackwardScan]}, ...additionalHints]
        };
        this._qsutils.withQuerySettings(querySettingsQuery, naturalBackwardSettings, () => {
            this.assertCollScanStage(query, ["backward"]);
            this.assertQuerySettingsInCacheForCommand(query, naturalBackwardSettings);
            additionalAssertions();
        });

        const naturalAnyDirectionSettings = {
            indexHints: [
                {ns, allowedIndexes: [naturalForwardScan, naturalBackwardScan]},
                ...additionalHints
            ]
        };
        this._qsutils.withQuerySettings(querySettingsQuery, naturalAnyDirectionSettings, () => {
            this.assertCollScanStage(query, ["forward", "backward"]);
            this.assertQuerySettingsInCacheForCommand(query, naturalAnyDirectionSettings);
            additionalAssertions();
        });
    }

    /**
     * Ensure that the hint gets ignored when query settings for the particular query are set.
     */
    assertQuerySettingsIgnoreCursorHints(querySettingsQuery, ns) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        const queryWithHint = {...query, hint: this.indexA};
        const settings = {indexHints: {ns, allowedIndexes: [this.indexAB]}};
        const getWinningPlansForQuery = (query) => {
            const explain = assert.commandWorked(this._db.runCommand({explain: query}));
            return getQueryPlanners(explain).map(getWinningPlan);
        };

        this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            const winningPlanWithoutCursorHint = getWinningPlansForQuery(query);
            const winningPlanWithCursorHint = getWinningPlansForQuery(queryWithHint);
            // In sharded explain, the winning plans on the shards aren't guaranteed to be returned
            // in a particular order, so check that the elements of the explain output are equal.
            assert.sameMembers(winningPlanWithCursorHint, winningPlanWithoutCursorHint);
        });
    }

    /**
     * Ensure that cursor hints and query settings can be applied together, if on independent
     * pipelines.
     */
    assertQuerySettingsWithCursorHints(querySettingsQuery, mainNs, secondaryNs) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        const queryWithHint = {...query, hint: this.indexA};
        const settingsOnSecondary = {indexHints: {ns: secondaryNs, allowedIndexes: [this.indexAB]}};
        const settingsOnBoth = {
            indexHints: [
                {ns: mainNs, allowedIndexes: [this.indexA]},
                {ns: secondaryNs, allowedIndexes: [this.indexAB]},
            ]
        };
        const getWinningPlansForQuery = (query, settings) => {
            let winningPlans = null;
            this._qsutils.withQuerySettings(
                {...query, $db: querySettingsQuery.$db}, settings, () => {
                    const explain = assert.commandWorked(this._db.runCommand({explain: query}));
                    winningPlans = getQueryPlanners(explain).map(getWinningPlan);
                });
            return winningPlans;
        };

        // In sharded explain, the winning plans on the shards aren't guaranteed to be returned in a
        // particular order, so check that the elements of the explain output are equal.
        assert.sameMembers(getWinningPlansForQuery(query, settingsOnBoth),
                           getWinningPlansForQuery(queryWithHint, settingsOnSecondary));
    }

    /**
     * Ensure that queries that fallback to multiplanning when the provided settings don't generate
     * any viable plans have the same winning plan as the queries that have no query settings
     * attached to them.
     */
    assertQuerySettingsFallback(querySettingsQuery, ns) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {ns, allowedIndexes: ["doesnotexist"]}};
        const getWinningStages = (explain) =>
            getQueryPlanners(explain).flatMap(getWinningPlan).flatMap(flattenQueryPlanTree);

        // It's not guaranteed for all the queries to preserve the order of the stages when
        // replanning (namely in the case of subplanning with $or statements). Flatten the plan tree
        // & check that the plans' elements are equal using `assert.sameMembers` to accommodate this
        // behavior and avoid potential failures.
        const explainCmd = getExplainCommand(query);
        const explainWithoutQuerySettings = assert.commandWorked(
            this._db.runCommand(explainCmd),
            `Failed running ${tojson(explainCmd)} before setting query settings`);
        const winningStagesWithoutQuerySettings = getWinningStages(explainWithoutQuerySettings);
        this._qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            const explainWithQuerySettings = assert.commandWorked(
                this._db.runCommand(explainCmd),
                `Failed running ${tojson(explainCmd)} after settings query settings`);
            const winningStagesWithQuerySettings = getWinningStages(explainWithQuerySettings);
            assert.sameMembers(
                winningStagesWithQuerySettings,
                winningStagesWithoutQuerySettings,
                "Expected the query without query settings and the one with settings to have " +
                    "identical plan stages.");
            assert.eq(
                explainWithQuerySettings.pipeline,
                explainWithoutQuerySettings.pipeline,
                "Expected the query without query settings and the one with settings to have " +
                    "identical pipelines.");
            this.assertQuerySettingsInCacheForCommand(query, settings);
        });
    }

    /**
     * Ensure that users can not pass query settings to the commands explicitly.
     */
    assertQuerySettingsCommandValidation(querySettingsQuery, ns) {
        const query = this._qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {ns, allowedIndexes: [this.indexAB]}};
        const expectedErrorCodes = [7746900, 7746901, 7923000, 7923001, 7708000, 7708001];
        assert.commandFailedWithCode(this._db.runCommand({...query, querySettings: settings}),
                                     expectedErrorCodes);
    }

    testAggregateQuerySettingsNaturalHintEquiJoinStrategy(query, mainNs, secondaryNs) {
        // Confirm that, by default, the query can be satisfied with an IndexedLoopJoin when joining
        // against the collection.
        const queryNoDb = this._qsutils.withoutDollarDB(query);
        this.assertLookupJoinStage(queryNoDb, undefined, false, "IndexedLoopJoin");

        // Set query settings, hinting $natural for the secondary collection.
        this._qsutils.withQuerySettings(
            query, {indexHints: [{ns: secondaryNs, allowedIndexes: [{"$natural": 1}]}]}, () => {
                // Confirm the strategy has changed - the query is no longer
                // permitted to use the index on the secondary collection.
                this.assertLookupJoinStage(queryNoDb, undefined, false, "HashJoin");
            });

        // Set query settings, but hinting $natural on the "main" collection. Strategy
        this._qsutils.withQuerySettings(
            query, {indexHints: [{ns: mainNs, allowedIndexes: [{"$natural": 1}]}]}, () => {
                // Observe that strategy is unaffected in this case; the top level query was
                // already a coll scan, and the query is allowed to use the index on the
                // secondary collection.
                this.assertLookupJoinStage(queryNoDb, undefined, false, "IndexedLoopJoin");
            });
    }

    testAggregateQuerySettingsNaturalHintDirectionWhenSecondaryHinted(
        query, mainNs, secondaryNs, lookupResultExtractor = (doc) => doc.output) {
        let params = [
            {hint: [{"$natural": 1}], cmp: (a, b) => a <= b},
            {hint: [{"$natural": -1}], cmp: (a, b) => a >= b},
            {hint: [{"$natural": 1}, {"$natural": -1}], cmp: () => true},
        ];

        for (const {hint, cmp} of params) {
            this.assertQuerySettingsNaturalApplication(
                query, mainNs, [{ns: secondaryNs, allowedIndexes: hint}], () => {
                    // The order of the documents in output should correspond to the $natural hint
                    // direction set for the secondary collection.
                    const res = assert.commandWorked(
                        this._db.runCommand(this._qsutils.withoutDollarDB(query)));
                    const docs = getAllDocuments(this._db, res);

                    for (const doc of docs) {
                        for (const [a, b] of pairwise(lookupResultExtractor(doc))) {
                            assert(cmp(a, b), {
                                msg: "$lookup result not in expected order",
                                docs: docs,
                                doc: doc
                            });
                        }
                    }
                });
        }
    }
}

function getAllDocuments(db, commandResult) {
    return (new DBCommandCursor(db, commandResult)).toArray();
}

function* pairwise(iterable) {
    const iterator = iterable[Symbol.iterator]();
    let a = iterator.next();
    if (a.done) {
        return;
    }
    let b = iterator.next();
    while (!b.done) {
        yield [a.value, b.value];
        a = b;
        b = iterator.next();
    }
}

function* crossProductGenerator(...lists) {
    const [head, ...tail] = lists;
    if (tail.length == 0) {
        yield* head;
        return;
    }

    for (const element of head) {
        for (const rest of crossProductGenerator(...tail)) {
            yield [element].concat(rest);
        }
    }
}

function crossProduct(...lists) {
    return [...crossProductGenerator(...lists)];
}

function selfCrossProduct(list) {
    return crossProduct(list, list);
}
