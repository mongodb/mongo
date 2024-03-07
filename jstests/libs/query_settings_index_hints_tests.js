import {
    getAggPlanStages,
    getPlanStages,
    getQueryPlanners,
    getWinningPlan
} from "jstests/libs/analyze_plan.js";
import {checkSbeFullyEnabled, checkSbeRestrictedOrFullyEnabled} from "jstests/libs/sbe_util.js";

/**
 * Class containing common test functions used in query_settings_index_application_* tests.
 */
export class QuerySettingsIndexHintsTests {
    /**
     * Create a query settings utility class.
     */
    constructor(qsutils) {
        this.qsutils = qsutils;
        this.indexA = {a: 1};
        this.indexB = {b: 1};
        this.indexAB = {a: 1, b: 1};
    }

    /**
     * Asserts that after executing 'command' the most recent query plan from cache would have
     * 'querySettings' set.
     */
    assertQuerySettingsInCacheForCommand(command, querySettings) {
        // Single solution plans are not cached in classic, therefore do not perform plan cache
        // checks for classic.
        const db = this.qsutils.db;
        if (!checkSbeFullyEnabled(db)) {
            return;
        }

        // Clear the plan cache before running any queries.
        db[this.qsutils.collName].getPlanCache().clear();

        // Take the newest plan cache entry (based on 'timeOfCreation' sorting) and ensure that it
        // contains the 'settings'.
        assert.commandWorked(db.runCommand(command));
        const planCacheStatsAfterRunningCmd =
            db[this.qsutils.collName].aggregate([{$planCacheStats: {}}]).toArray();
        assert.gte(planCacheStatsAfterRunningCmd.length,
                   1,
                   "Expecting at least 1 entry in query plan cache");
        planCacheStatsAfterRunningCmd.forEach(
            plan => assert.docEq(plan.querySettings, querySettings, plan));
    }

    assertIndexUse(cmd, expectedIndex, stagesExtractor) {
        const explain = assert.commandWorked(db.runCommand({explain: cmd}));
        const stagesUsingIndex = stagesExtractor(explain);
        assert.gte(stagesUsingIndex.length, 1, explain);
        if (expectedIndex == undefined) {
            // Don't verify index name.
            return;
        }
        for (const stage of stagesUsingIndex) {
            assert.docEq(stage.keyPattern, expectedIndex, explain);
        }
    }

    assertIndexScanStage(cmd, expectedIndex) {
        return this.assertIndexUse(cmd, expectedIndex, (explain) => {
            return getQueryPlanners(explain)
                .map(getWinningPlan)
                .flatMap(winningPlan => getPlanStages(winningPlan, "IXSCAN"));
        });
    }

    assertLookupJoinStage(cmd, expectedIndex) {
        // $lookup stage is only pushed down to find in SBE and not in classic.
        const db = this.qsutils.db;
        if (!checkSbeRestrictedOrFullyEnabled(db)) {
            return this.assertLookupPipelineStage(cmd, expectedIndex);
        }

        return this.assertIndexUse(cmd, expectedIndex, (explain) => {
            return getQueryPlanners(explain)
                .map(getWinningPlan)
                .flatMap(winningPlan => getPlanStages(winningPlan, "EQ_LOOKUP"))
                .map(stage => {
                    stage.keyPattern = stage.indexKeyPattern;
                    return stage;
                });
        });
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
        const explain = assert.commandWorked(this.qsutils.db.runCommand({explain: cmd}));
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
    assertQuerySettingsIndexApplication(
        querySettingsQuery, ns = {db: this.qsutils.db.getName(), coll: this.qsutils.collName}) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexB, this.indexAB]) {
            const settings = {indexHints: {ns, allowedIndexes: [index]}};
            this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, index);
                this.assertQuerySettingsInCacheForCommand(query, settings);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of the equi-join over namespace 'ns'.
     */
    assertQuerySettingsLookupJoinIndexApplication(querySettingsQuery, ns) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexAB]) {
            const settings = {indexHints: {ns, allowedIndexes: [index]}};
            this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertLookupJoinStage(query, index);
                this.assertQuerySettingsInCacheForCommand(query, settings);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of $lookup equi-join for both collections.
     */
    assertQuerySettingsIndexAndLookupJoinApplications(querySettingsQuery, mainNs, secondaryNs) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        for (const [mainCollIndex, secondaryCollIndex] of crossProduct(
                 [this.indexA, this.indexAB])) {
            const settings = {
                indexHints: [
                    {ns: mainNs, allowedIndexes: [mainCollIndex]},
                    {ns: secondaryNs, allowedIndexes: [secondaryCollIndex]},
                ]
            };

            this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, mainCollIndex);
                this.assertLookupJoinStage(query, secondaryCollIndex);
                this.assertQuerySettingsInCacheForCommand(query, settings);
            });
        }
    }

    /**
     * Ensure query settings are applied in a situation of $lookup sub-pipeline.
     */
    assertQuerySettingsLookupPipelineIndexApplication(querySettingsQuery, ns) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        for (const index of [this.indexA, this.indexB, this.indexAB]) {
            const settings = {indexHints: {ns, allowedIndexes: [index]}};
            this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
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
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        for (const [mainCollIndex, secondaryCollIndex] of crossProduct(
                 [this.indexA, this.indexB, this.indexAB])) {
            const settings = {
                indexHints: [
                    {ns: mainNs, allowedIndexes: [mainCollIndex]},
                    {ns: secondaryNs, allowedIndexes: [secondaryCollIndex]},
                ]
            };

            this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
                this.assertIndexScanStage(query, mainCollIndex);
                this.assertLookupPipelineStage(query, secondaryCollIndex);
                this.assertQuerySettingsInCacheForCommand(query, settings);
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
    assertQuerySettingsNaturalApplication(querySettingsQuery) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const naturalForwardScan = {$natural: 1};
        const naturalForwardSettings = {indexHints: {allowedIndexes: [naturalForwardScan]}};
        this.qsutils.withQuerySettings(querySettingsQuery, naturalForwardSettings, () => {
            this.assertCollScanStage(query, ["forward"]);
            this.assertQuerySettingsInCacheForCommand(query, naturalForwardSettings);
        });

        const naturalBackwardScan = {$natural: -1};
        const naturalBackwardSettings = {indexHints: {allowedIndexes: [naturalBackwardScan]}};
        this.qsutils.withQuerySettings(querySettingsQuery, naturalBackwardSettings, () => {
            this.assertCollScanStage(query, ["backward"]);
            this.assertQuerySettingsInCacheForCommand(query, naturalBackwardSettings);
        });

        const naturalAnyDirectionSettings = {
            indexHints: {allowedIndexes: [naturalForwardScan, naturalBackwardScan]}
        };
        this.qsutils.withQuerySettings(querySettingsQuery, naturalAnyDirectionSettings, () => {
            this.assertCollScanStage(query, ["forward", "backward"]);
            this.assertQuerySettingsInCacheForCommand(query, naturalAnyDirectionSettings);
        });
    }

    /**
     * Ensure that the hint gets ignored when query settings for the particular query are set.
     */
    assertQuerySettingsIgnoreCursorHints(querySettingsQuery, ns) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const queryWithHint = {...query, hint: this.indexA};
        const settings = {indexHints: {ns, allowedIndexes: [this.indexAB]}};
        const getWinningPlansForQuery = (query) => {
            const explain = assert.commandWorked(db.runCommand({explain: query}));
            return getQueryPlanners(explain).map(getWinningPlan);
        };

        this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            const winningPlanWithoutCursorHint = getWinningPlansForQuery(query);
            const winningPlanWithCursorHint = getWinningPlansForQuery(queryWithHint);
            assert.eq(winningPlanWithCursorHint, winningPlanWithoutCursorHint);
        });
    }

    /**
     * Ensure that cursor hints and query settings can be applied together, if on independent
     * pipelines.
     */
    assertQuerySettingsWithCursorHints(querySettingsQuery, mainNs, secondaryNs) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
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
            this.qsutils.withQuerySettings(
                {...query, $db: querySettingsQuery.$db}, settings, () => {
                    const explain = assert.commandWorked(db.runCommand({explain: query}));
                    winningPlans = getQueryPlanners(explain).map(getWinningPlan);
                });
            return winningPlans;
        };

        assert.eq(getWinningPlansForQuery(query, settingsOnBoth),
                  getWinningPlansForQuery(queryWithHint, settingsOnSecondary));
    }

    /**
     * Ensure that queries that fallback to multiplanning when the provided settings don't generate
     * any viable plans have the same winning plan as the queries that have no query settings
     * attached to them.
     */
    assertQuerySettingsFallback(querySettingsQuery,
                                ns = {db: this.qsutils.db.getName(), coll: this.qsutils.collName}) {
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {ns, allowedIndexes: ["doesnotexist"]}};
        const explainWithoutQuerySettings = assert.commandWorked(db.runCommand({explain: query}));

        this.qsutils.withQuerySettings(querySettingsQuery, settings, () => {
            const explainWithQuerySettings = assert.commandWorked(db.runCommand({explain: query}));
            const winningPlansWithQuerySettings =
                getQueryPlanners(explainWithQuerySettings).map(getWinningPlan);
            const winningPlansWithoutQuerySettings =
                getQueryPlanners(explainWithoutQuerySettings).map(getWinningPlan);
            assert.eq(winningPlansWithQuerySettings, winningPlansWithoutQuerySettings);
            this.assertQuerySettingsInCacheForCommand(query, settings);
        });
    }

    /**
     * Ensure that users can not pass query settings to the commands explicitly.
     */
    assertQuerySettingsCommandValidation(querySettingsQuery) {
        const ns = {db: this.qsutils.db.getName(), coll: this.qsutils.collName};
        const query = this.qsutils.withoutDollarDB(querySettingsQuery);
        const settings = {indexHints: {ns, allowedIndexes: [this.indexAB]}};
        const expectedErrorCodes = [7746900, 7746901, 7923000, 7923001, 7708000, 7708001];
        assert.commandFailedWithCode(
            this.qsutils.db.runCommand({...query, querySettings: settings}), expectedErrorCodes);
    }
}

function crossProduct(list) {
    let result = [];
    for (let i = 0; i < list.length; i++) {
        for (let j = 0; j < list.length; j++) {
            result.push([list[i], list[j]]);
        }
    }
    return result;
}
