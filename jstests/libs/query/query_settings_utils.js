/**
 * Utility class for testing query settings.
 */
import {getCommandName, getExplainCommand} from "jstests/libs/cmd_object_utils.js";
import {
    getAggPlanStages,
    getEngine,
    getPlanStages,
    getQueryPlanners,
    getWinningPlanFromExplain
} from "jstests/libs/query/analyze_plan.js";

export class QuerySettingsUtils {
    /**
     * Create a query settings utility class.
     */
    constructor(db, collName) {
        this._db = db;
        this._adminDB = this._db.getSiblingDB("admin");
        this._collName = collName;
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
            ...aggregateObj
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
    getQuerySettings({showDebugQueryShape = false,
                      showQueryShapeHash = false,
                      filter = undefined} = {}) {
        const pipeline = [{$querySettings: showDebugQueryShape ? {showDebugQueryShape} : {}}];
        if (filter) {
            pipeline.push({$match: filter});
        }
        if (!showQueryShapeHash) {
            pipeline.push({$project: {queryShapeHash: 0}});
        }
        pipeline.push({$sort: {representativeQuery: 1}});
        return this._adminDB.aggregate(pipeline).toArray();
    }

    /**
     * Return 'queryShapeHash' for a given query from 'querySettings'.
     */
    getQueryShapeHashFromQuerySettings(representativeQuery) {
        const settings =
            this.getQuerySettings({showQueryShapeHash: true, filter: {representativeQuery}});
        assert.lte(
            settings.length,
            1,
            `query ${tojson(representativeQuery)} is expected to have 0 or 1 settings, but got ${
                tojson(settings)}`);
        return settings.length === 0 ? undefined : settings[0].queryShapeHash;
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
    assertQueryShapeConfiguration(expectedQueryShapeConfigurations, shouldRunExplain = true) {
        const rewrittenExpectedQueryShapeConfigurations =
            expectedQueryShapeConfigurations.map(config => {
                return {...config, settings: this.wrapIndexHintsIntoArrayIfNeeded(config.settings)};
            });
        assert.soon(
            () => {
                let currentQueryShapeConfigurationWo = this.getQuerySettings();
                currentQueryShapeConfigurationWo.sort(bsonWoCompare);
                rewrittenExpectedQueryShapeConfigurations.sort(bsonWoCompare);
                return bsonWoCompare(currentQueryShapeConfigurationWo,
                                     rewrittenExpectedQueryShapeConfigurations) == 0;
            },
            "current query settings = " + tojson(this.getQuerySettings()) +
                ", expected query settings = " + tojson(rewrittenExpectedQueryShapeConfigurations));

        if (shouldRunExplain) {
            const settingsArray = this.getQuerySettings({showQueryShapeHash: true});
            for (const {representativeQuery, settings, queryShapeHash} of settingsArray) {
                this.assertExplainQuerySettings(representativeQuery, settings, queryShapeHash);
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
            getQueryPlanners(explain).forEach(queryPlanner => {
                this.assertEqualSettings(
                    expectedQuerySettings, queryPlanner.querySettings, queryPlanner);
            });

            if (expectedQueryShapeHash) {
                const {queryShapeHash} = explain;
                assert.eq(queryShapeHash, expectedQueryShapeHash);
            }
        }
    }

    /**
     * Remove all query settings for the current tenant.
     */
    removeAllQuerySettings() {
        let settingsArray = this.getQuerySettings({showQueryShapeHash: true});
        while (settingsArray.length > 0) {
            const setting = settingsArray.pop();
            assert.commandWorked(
                this._adminDB.runCommand({removeQuerySettings: setting.queryShapeHash}));
        }
        // Check that all setting have indeed been removed.
        this.assertQueryShapeConfiguration([]);
    }

    /**
     * Helper method for setting & removing query settings for testing purposes. Accepts a
     * 'runTest' anonymous function which will be executed once the provided query settings have
     * been propagated throughout the cluster.
     */
    withQuerySettings(representativeQuery, settings, runTest) {
        let queryShapeHash = undefined;
        try {
            const setQuerySettingsCmd = {setQuerySettings: representativeQuery, settings: settings};
            queryShapeHash =
                assert.commandWorked(this._db.adminCommand(setQuerySettingsCmd)).queryShapeHash;
            assert.soon(() => (this.getQuerySettings({filter: {queryShapeHash}}).length === 1));
            return runTest();
        } finally {
            if (queryShapeHash) {
                const removeQuerySettingsCmd = {removeQuerySettings: representativeQuery};
                assert.commandWorked(this._db.adminCommand(removeQuerySettingsCmd));
                assert.soon(() => (this.getQuerySettings({filter: {queryShapeHash}}).length === 0));
            }
        }
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
        assert.docEq(this.wrapIndexHintsIntoArrayIfNeeded(lhs),
                     this.wrapIndexHintsIntoArrayIfNeeded(rhs),
                     message);
    }

    /**
     * Asserts that the expected engine is run on the input query and settings.
     */
    assertQueryFramework({query, settings, expectedEngine}) {
        // Ensure that query settings cluster parameter is empty.
        this.assertQueryShapeConfiguration([]);

        // Apply the provided settings for the query.
        if (settings) {
            assert.commandWorked(
                this._db.adminCommand({setQuerySettings: query, settings: settings}));
            // Wait until the settings have taken effect.
            const expectedConfiguration = [this.makeQueryShapeConfiguration(settings, query)];
            this.assertQueryShapeConfiguration(expectedConfiguration);
        }

        const withoutDollarDB = query.aggregate ? {...this.withoutDollarDB(query), cursor: {}}
                                                : this.withoutDollarDB(query);
        const explain = assert.commandWorked(this._db.runCommand({explain: withoutDollarDB}));
        const engine = getEngine(explain);
        assert.eq(
            engine, expectedEngine, `Expected engine to be ${expectedEngine} but found ${engine}`);

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
}
