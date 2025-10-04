/**
 * Utility class for testing query settings.
 */
import {getCommandName, getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    getAggPlanStages,
    getEngine,
    getPlanStages,
    getQueryPlanners,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {getParameter, setParameterOnAllHosts} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

export class QuerySettingsUtils {
    /**
     * Create a query settings utility class.
     */
    constructor(db, collName) {
        this._db = db;
        this._adminDB = this._db.getSiblingDB("admin");
        this._collName = collName;
        this._onSetQuerySettingsHooks = [];
    }

    /**
     * Returns 'true' if the given command name is supported by query settings.
     */
    static isSupportedCommand(commandName) {
        return ["find", "aggregate", "distinct"].includes(commandName);
    }

    /**
     * Makes an query instance for the given command if supported.
     */
    makeQueryInstance(cmdObj) {
        const commandName = getCommandName(cmdObj);
        switch (commandName) {
            case "find":
                return this.makeFindQueryInstance(cmdObj);
            case "aggregate":
                return this.makeAggregateQueryInstance(cmdObj);
            case "distinct":
                return this.makeDistinctQueryInstance(cmdObj);
            default:
                assert(false, "Cannot create query instance for command with name " + commandName);
        }
    }

    /**
     * Makes an query instance of the find command.
     */
    makeFindQueryInstance(findObj) {
        return {find: this._collName, $db: this._db.getName(), ...findObj};
    }

    /**
     * Makes a query instance of the distinct command.
     */
    makeDistinctQueryInstance(distinctObj) {
        return {distinct: this._collName, $db: this._db.getName(), ...distinctObj};
    }

    /**
     * Makes a query instance of the aggregate command with an optional pipeline clause.
     */
    makeAggregateQueryInstance(aggregateObj, collectionless = false) {
        return {
            aggregate: collectionless ? 1 : this._collName,
            $db: this._db.getName(),
            cursor: {},
            ...aggregateObj,
        };
    }

    /**
     * Makes a QueryShapeConfiguration object without the QueryShapeHash.
     */
    makeQueryShapeConfiguration(settings, representativeQuery) {
        return {settings, representativeQuery};
    }

    makeSetQuerySettingsCommand({settings, representativeQuery}) {
        return {setQuerySettings: representativeQuery, settings};
    }

    makeRemoveQuerySettingsCommand(representativeQuery) {
        return {removeQuerySettings: representativeQuery};
    }

    /**
     * Return query settings for the current tenant without query shape hashes.
     */
    getQuerySettings({
        showDebugQueryShape = false,
        showQueryShapeHash = false,
        showRepresentativeQuery = true,
        ignoreRepresentativeQueryFields = [],
        filter = undefined,
    } = {}) {
        const pipeline = [{$querySettings: showDebugQueryShape ? {showDebugQueryShape} : {}}];
        if (filter) {
            pipeline.push({$match: filter});
        }
        if (!showQueryShapeHash) {
            pipeline.push({$project: {queryShapeHash: 0}});
        }
        for (const ignoredField of ignoreRepresentativeQueryFields) {
            pipeline.push({
                $replaceWith: {
                    $setField: {
                        field: "representativeQuery",
                        input: "$$ROOT",
                        value: {
                            $unsetField: {
                                field: {$literal: ignoredField},
                                input: {
                                    $getField: "representativeQuery",
                                },
                            },
                        },
                    },
                },
            });
        }
        pipeline.push({$sort: {representativeQuery: 1}});
        if (!showRepresentativeQuery) {
            pipeline.push({$project: {representativeQuery: 0}});
        }
        return this._adminDB.aggregate(pipeline).toArray();
    }

    /**
     * Return 'queryShapeHash' for a given query from 'querySettings'.
     */
    getQueryShapeHashFromQuerySettings(representativeQuery) {
        const settings = this.getQuerySettings({showQueryShapeHash: true, filter: {representativeQuery}});
        assert.lte(
            settings.length,
            1,
            `query ${tojson(representativeQuery)} is expected to have 0 or 1 settings, but got ${tojson(settings)}`,
        );
        return settings.length === 0 ? undefined : settings[0].queryShapeHash;
    }

    /**
     * Return 'queryShapeHash' for a given 'representativeQuery' by calling explain over it.
     */
    getQueryShapeHashFromExplain(representativeQuery) {
        const explainCmd = getExplainCommand(this.withoutDollarDB(representativeQuery));
        const explain = assert.commandWorked(this._db.runCommand(explainCmd));
        return explain.queryShapeHash;
    }

    /**
     * Return the query settings section of the server status.
     */
    getQuerySettingsServerStatus() {
        return assert.commandWorked(this._db.runCommand({serverStatus: 1})).querySettings;
    }

    /**
     * Helper function to assert equality of QueryShapeConfigurations. In order to ease the
     * assertion logic, 'queryShapeHash' field is removed from the QueryShapeConfiguration prior
     * to assertion.
     *
     * Since in sharded clusters the query settings may arrive with a delay to the mongos, the
     * assertion is done via 'assert.soon'.
     *
     * The settings list is not expected to be in any particular order.
     */
    assertQueryShapeConfiguration(
        expectedQueryShapeConfigurations,
        shouldRunExplain = true,
        ignoreRepresentativeQueryFields = [],
    ) {
        const isRunningFCVUpgradeDowngradeSuite = TestData.isRunningFCVUpgradeDowngradeSuite || false;

        // In case 'expectedQueryShapeConfigurations' has no 'representativeQuery' attribute, we do
        // not perform 'queryShapeHash' assertions.
        const expectedQueryShapeConfigurationsHaveRepresentativeQuery = expectedQueryShapeConfigurations.every(
            (config) => config.hasOwnProperty("representativeQuery"),
        );
        let showQueryShapeHash =
            expectedQueryShapeConfigurationsHaveRepresentativeQuery && isRunningFCVUpgradeDowngradeSuite;
        const rewrittenExpectedQueryShapeConfigurations = expectedQueryShapeConfigurations.map((config) => {
            const {settings, representativeQuery} = config;
            const rewrittenSettings = this.wrapIndexHintsIntoArrayIfNeeded(settings);
            if (!isRunningFCVUpgradeDowngradeSuite || !representativeQuery) {
                return {...config, settings: rewrittenSettings};
            }

            // If running in FCV upgrade/downgrade suites, the 'representativeQuery' may be
            // missing. In that case we avoid asserting for 'representativeQuery' equality.
            // Instead, we ensure that queryShapeHashes are same.
            return {
                queryShapeHash: this.getQueryShapeHashFromExplain(representativeQuery),
                settings: rewrittenSettings,
            };
        });

        assert.soonNoExcept(
            () => {
                const actualQueryShapeConfigurations = this.getQuerySettings({
                    showQueryShapeHash,
                    ignoreRepresentativeQueryFields,
                    showRepresentativeQuery: !isRunningFCVUpgradeDowngradeSuite,
                });
                assert.sameMembers(actualQueryShapeConfigurations, rewrittenExpectedQueryShapeConfigurations);
                return true;
            },
            () =>
                `current query settings = ${toJsonForLog(
                    this.getQuerySettings({
                        showQueryShapeHash: true,
                    }),
                )}, expected query settings = ${toJsonForLog(rewrittenExpectedQueryShapeConfigurations)}`,
        );

        if (shouldRunExplain && expectedQueryShapeConfigurationsHaveRepresentativeQuery) {
            const settingsArray = this.getQuerySettings({showQueryShapeHash, ignoreRepresentativeQueryFields});
            for (const {representativeQuery, settings, queryShapeHash} of settingsArray) {
                if (representativeQuery) {
                    this.assertExplainQuerySettings(representativeQuery, settings, queryShapeHash);
                }
            }
        }
    }

    /**
     * Asserts that the explain output for 'query' contains 'expectedQuerySettings' and
     * 'expectedQueryShapeHash'.
     */
    assertExplainQuerySettings(query, expectedQuerySettings, expectedQueryShapeHash = undefined) {
        // Pass query without the $db field to explain command, because it injects the $db field
        // inside the query before processing.
        const explainCmd = getExplainCommand(this.withoutDollarDB(query));
        const explain = assert.commandWorked(this._db.runCommand(explainCmd));
        if (explain) {
            getQueryPlanners(explain).forEach((queryPlanner) => {
                this.assertEqualSettings(expectedQuerySettings, queryPlanner.querySettings, queryPlanner);
            });

            if (expectedQueryShapeHash) {
                const {queryShapeHash} = explain;
                assert.eq(queryShapeHash, expectedQueryShapeHash);
            }
        }
    }

    /**
     * Returns the representative queries stored in the 'queryShapeRepresentativeQueries'
     * collection.
     */
    getRepresentativeQueries() {
        // In sharded clusters, the representative queries are stored in the config server. So we
        // need to return the connection to the node that stores them.
        const nodeThatStoresRepresentativeQueries = (function (db) {
            const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
            if (topology.configsvr) {
                return new Mongo(topology.configsvr.nodes[0]);
            }
            return db.getMongo();
        })(this._db);

        return nodeThatStoresRepresentativeQueries
            .getDB("config")
            .queryShapeRepresentativeQueries.aggregate([{$replaceRoot: {newRoot: "$representativeQuery"}}])
            .toArray();
    }

    /**
     * Asserts the 'expectedRepresentativeQueries' are present in the
     * 'queryShapeRepresentativeQueries' collection.
     */
    assertRepresentativeQueries(expectedRepresentativeQueries) {
        assert.sameMembers(this.getRepresentativeQueries(), expectedRepresentativeQueries);
    }

    /**
     * Remove all query settings for the current tenant.
     */
    removeAllQuerySettings() {
        let settingsArray = this.getQuerySettings({showQueryShapeHash: true});
        while (settingsArray.length > 0) {
            const setting = settingsArray.pop();
            assert.commandWorked(this._adminDB.runCommand({removeQuerySettings: setting.queryShapeHash}));
        }
        // Check that all setting have indeed been removed.
        this.assertQueryShapeConfiguration([]);
    }

    /**
     * Helper method for setting & removing query settings for testing purposes. Accepts a
     * 'runTest' anonymous function which will be executed once the provided query settings have
     * been propagated throughout the cluster.
     */
    withQuerySettings(setQuerySettings, settings, runTest) {
        let queryShapeHash = undefined;
        let representativeQuery = undefined;
        try {
            const setQuerySettingsCmd = {setQuerySettings, settings};
            const response = assert.commandWorked(this._db.adminCommand(setQuerySettingsCmd));
            queryShapeHash = response.queryShapeHash;
            representativeQuery = response.representativeQuery;

            // Assert that the 'expectedQueryShapeConfiguration' is present in the system.
            const expectedQueryShapeConfiguration = {queryShapeHash, settings: response.settings};
            if (representativeQuery) {
                expectedQueryShapeConfiguration.representativeQuery = representativeQuery;
            }
            assert.soonNoExcept(() => {
                const settings = this.getQuerySettings({filter: {queryShapeHash}, showQueryShapeHash: true});
                assert.sameMembers(settings, [expectedQueryShapeConfiguration]);
                return true;
            });

            this._onSetQuerySettingsHooks.forEach((hook) => hook());
            return runTest();
        } finally {
            if (queryShapeHash) {
                const removeQuerySettingsCmd = {
                    removeQuerySettings: representativeQuery ?? queryShapeHash,
                };
                assert.commandWorked(this._db.adminCommand(removeQuerySettingsCmd));
                assert.soon(() => this.getQuerySettings({filter: {queryShapeHash}}).length === 0);
            }
        }
    }

    withFailpoint(failPointName, data, fn) {
        // 'coordinator' corresponds to replset primary in replica set or configvr primary in
        // sharded clusters.
        const coordinator = (function (db) {
            const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
            const hasMongosThatForwardsQuerySettingsCmdsToConfigsvr =
                MongoRunner.compareBinVersions(jsTestOptions().mongosBinVersion, "8.2") >= 0;
            if (topology.configsvr && hasMongosThatForwardsQuerySettingsCmdsToConfigsvr) {
                return new Mongo(topology.configsvr.nodes[0]);
            }
            return db.getMongo();
        })(this._db);

        const failpoint = configureFailPoint(coordinator, failPointName, data);
        try {
            return fn(failpoint, coordinator.port);
        } finally {
            failpoint.off();
        }
    }

    /**
     * Helper method for temporarely setting the 'internalQuerySettingsBackfillDelaySeconds' server
     * parameter to 'delaySeconds' while executing the provided 'fn'. Restores the original value
     * upon completion.
     */
    withBackfillDelaySeconds(delaySeconds, fn) {
        let originalDelaySeconds = null;
        let hostList = [];
        try {
            const conn = this._db.getMongo();
            hostList = DiscoverTopology.findNonConfigNodes(conn);
            assert.gt(hostList.length, 0, "No hosts found");
            originalDelaySeconds = getParameter(conn, "internalQuerySettingsBackfillDelaySeconds");
            setParameterOnAllHosts(hostList, "internalQuerySettingsBackfillDelaySeconds", delaySeconds);
            return fn();
        } finally {
            if (hostList.length > 0 && originalDelaySeconds !== null) {
                setParameterOnAllHosts(hostList, "internalQuerySettingsBackfillDelaySeconds", originalDelaySeconds);
            }
        }
    }

    /**
     * Register a hook to be executed after the "setQuerySettings" command on every
     * withQuerySettings() invocation.
     */
    onSetQuerySettings(hook) {
        this._onSetQuerySettingsHooks.push(hook);
    }

    withoutDollarDB(cmd) {
        const {$db: _, ...rest} = cmd;
        return rest;
    }

    /**
     * 'indexHints' as part of query settings may be passed as object or as array. On the server the
     * indexHints will always be transformed into an array. For correct comparison, wrap
     * 'indexHints' into array if they are not array already.
     */
    wrapIndexHintsIntoArrayIfNeeded(settings) {
        if (!settings) {
            return settings;
        }

        let result = Object.assign({}, settings);
        if (result.indexHints && !Array.isArray(result.indexHints)) {
            result.indexHints = [result.indexHints];
        }
        return result;
    }

    /**
     * Asserts query settings by using wrapIndexHintsIntoArrayIfNeeded() helper method to ensure
     * that the settings are in the same format as seen by the server.
     */
    assertEqualSettings(lhs, rhs, message) {
        assert.docEq(this.wrapIndexHintsIntoArrayIfNeeded(lhs), this.wrapIndexHintsIntoArrayIfNeeded(rhs), message);
    }

    /**
     * Asserts that the expected engine is run on the input query and settings.
     */
    assertQueryFramework({query, settings, expectedEngine}) {
        // Ensure that query settings cluster parameter is empty.
        this.assertQueryShapeConfiguration([]);

        // Apply the provided settings for the query.
        if (settings) {
            assert.commandWorked(this._db.adminCommand({setQuerySettings: query, settings: settings}));
            // Wait until the settings have taken effect.
            const expectedConfiguration = [this.makeQueryShapeConfiguration(settings, query)];
            this.assertQueryShapeConfiguration(expectedConfiguration);
        }

        const explainCmd = getExplainCommand(this.withoutDollarDB(query));
        const explain = assert.commandWorked(this._db.runCommand(explainCmd));
        const engine = getEngine(explain);
        assert.eq(engine, expectedEngine, `Expected engine to be ${expectedEngine} but found ${engine}`);

        // Ensure that no $cursor stage exists, which means the whole query got pushed down to find,
        // if 'expectedEngine' is SBE.
        if (query.aggregate) {
            const cursorStages = getAggPlanStages(explain, "$cursor");

            if (expectedEngine === "sbe") {
                assert.eq(cursorStages.length, 0, cursorStages);
            } else {
                assert.gte(cursorStages.length, 0, cursorStages);
            }
        }

        // If a hinted index exists, assert it was used.
        if (query.hint) {
            const winningPlan = getWinningPlanFromExplain(explain);
            const ixscanStage = getPlanStages(winningPlan, "IXSCAN")[0];
            assert.eq(query.hint, ixscanStage.keyPattern, winningPlan);
        }

        this.removeAllQuerySettings();
    }

    /**
     * Tests that setting `reject` fails the expected query `query`, and a query with the same
     * shape, `queryPrime`, and does _not_ fail a query of differing shape, `unrelatedQuery`.
     */
    assertRejection({query, queryPrime, unrelatedQuery}) {
        // Confirm there's no pre-existing settings.
        this.assertQueryShapeConfiguration([]);

        const type = Object.keys(query)[0];
        const getRejectCount = () => db.runCommand({serverStatus: 1}).metrics.commands[type].rejected;

        const rejectBaseline = getRejectCount();

        const assertRejectedDelta = (delta) => {
            let actual;
            assert.soon(
                () => (actual = getRejectCount()) == delta + rejectBaseline,
                () =>
                    tojson({
                        expected: delta + rejectBaseline,
                        actual: actual,
                        cmdType: type,
                        cmdMetrics: db.runCommand({serverStatus: 1}).metrics.commands[type],
                        metrics: db.runCommand({serverStatus: 1}).metrics,
                    }),
            );
        };

        const getFailedCount = () => db.runCommand({serverStatus: 1}).metrics.commands[type].failed;

        query = this.withoutDollarDB(query);
        queryPrime = this.withoutDollarDB(queryPrime);
        unrelatedQuery = this.withoutDollarDB(unrelatedQuery);

        for (const q of [query, queryPrime, unrelatedQuery]) {
            // With no settings, all queries should succeed.
            assert.commandWorked(db.runCommand(q));

            // And so should explaining those queries.
            assert.commandWorked(db.runCommand(getExplainCommand(q)));
        }

        // Still nothing has been rejected.
        assertRejectedDelta(0);

        // Set reject flag for query under test.
        assert.commandWorked(
            db.adminCommand({setQuerySettings: {...query, $db: db.getName()}, settings: {reject: true}}),
        );

        // Confirm settings updated.
        this.assertQueryShapeConfiguration(
            [this.makeQueryShapeConfiguration({reject: true}, {...query, $db: db.getName()})],
            /* shouldRunExplain */ true,
        );

        // Just setting the reject flag should not alter the rejected cmd counter.
        assertRejectedDelta(0);

        // Verify other query with same shape has those settings applied too.
        this.assertExplainQuerySettings({...queryPrime, $db: db.getName()}, {reject: true});

        // Explain should not alter the rejected cmd counter.
        assertRejectedDelta(0);

        const failedBaseline = getFailedCount();
        // The queries with the same shape should both _fail_.
        assert.commandFailedWithCode(db.runCommand(query), ErrorCodes.QueryRejectedBySettings);
        assertRejectedDelta(1);
        assert.commandFailedWithCode(db.runCommand(queryPrime), ErrorCodes.QueryRejectedBySettings);
        assertRejectedDelta(2);

        // Despite some rejections occurring, there should not have been any failures.
        assert.eq(failedBaseline, getFailedCount());

        // Unrelated query should succeed.
        assert.commandWorked(db.runCommand(unrelatedQuery));

        for (const q of [query, queryPrime, unrelatedQuery]) {
            // All explains should still succeed.
            assert.commandWorked(db.runCommand(getExplainCommand(q)));
        }

        // Explains still should not alter the cmd rejected counter.
        assertRejectedDelta(2);

        // Remove the setting.
        this.removeAllQuerySettings();
        this.assertQueryShapeConfiguration([]);

        // Once again, all queries should succeed.
        for (const q of [query, queryPrime, unrelatedQuery]) {
            assert.commandWorked(db.runCommand(q));
            assert.commandWorked(db.runCommand(getExplainCommand(q)));
        }

        // Successful, non-rejected queries should not alter the rejected cmd counter.
        assertRejectedDelta(2);
    }
}
