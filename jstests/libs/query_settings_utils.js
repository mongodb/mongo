/**
 * Utility class for testing query settings.
 */
import {getQueryPlanners} from "jstests/libs/analyze_plan.js";

export class QuerySettingsUtils {
    /**
     * Create a query settings utility class.
     */
    constructor(db, collName) {
        this.db = db;
        this.adminDB = this.db.getSiblingDB("admin");
        this.collName = collName;
    }

    /**
     * Makes an query instance of the find command.
     */
    makeFindQueryInstance(findObj) {
        return {find: this.collName, $db: this.db.getName(), ...findObj};
    }

    /**
     * Makes a query instance of the distinct command.
     */
    makeDistinctQueryInstance(distinctObj) {
        return {distinct: this.collName, $db: this.db.getName(), ...distinctObj};
    }

    /**
     * Makes an query instance of the aggregate command with an optional pipeline clause.
     */
    makeAggregateQueryInstance(aggregateObj, collectionless = false) {
        return {
            aggregate: collectionless ? 1 : this.collName,
            $db: this.db.getName(),
            ...aggregateObj
        };
    }

    /**
     * Makes a QueryShapeConfiguration object without the QueryShapeHash.
     */
    makeQueryShapeConfiguration(settings, representativeQuery) {
        return {settings, representativeQuery};
    }

    /**
     * Return query settings for the current tenant without query hashes.
     */
    getQuerySettings(opts = {}) {
        return this.adminDB
            .aggregate([
                {$querySettings: opts},
                {$project: {queryShapeHash: 0}},
                {$sort: {representativeQuery: 1}},
            ])
            .toArray();
    }

    /**
     * Return queryShapeHash for a given query from querySettings.
     */
    getQueryHashFromQuerySettings(shape) {
        print("looking for hash for the shape: \n" + tojson(shape))
        const settings = this.adminDB
                             .aggregate([
                                 {$querySettings: {showDebugQueryShape: true}},
                                 {$project: {"debugQueryShape.cmdNs": 0}},
                                 {$match: {debugQueryShape: shape}},
                             ])
                             .toArray();
        if (settings.length == 0) {
            const allSettings = this.adminDB
                                    .aggregate([
                                        {$querySettings: {showDebugQueryShape: true}},
                                        {$project: {"debugQueryShape.cmdNs": 0}},
                                    ])
                                    .toArray();
            print("Couldn't find any. All settings:\n" + tojson(allSettings));
            return undefined;
        }
        assert.eq(settings.length, 1);
        return settings[0].queryShapeHash;
    }

    /**
     * Return the query settings section of the server status.
     */
    getQuerySettingsServerStatus() {
        return this.db.runCommand({serverStatus: 1}).querySettings;
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
        assert.soon(
            () => {
                const current = this.getQuerySettings().map(x => tojson(x)).sort();
                const expected = expectedQueryShapeConfigurations.map(x => tojson(x)).sort();
                return current.length == expected.length &&
                    current.every((v, i, a) => v == expected[i]);
            },
            "current query settings = " + tojson(this.getQuerySettings()) +
                ", expected query settings = " + tojson(expectedQueryShapeConfigurations));

        if (shouldRunExplain) {
            for (let {representativeQuery, settings} of expectedQueryShapeConfigurations) {
                this.assertExplainQuerySettings(representativeQuery, settings);
            }
        }
    }

    /**
     * Asserts that the explain output for 'query' contains 'expectedQuerySettings'.
     */
    assertExplainQuerySettings(query, expectedQuerySettings) {
        // Pass query without the $db field to explain command, because it injects the $db field
        // inside the query before processing.
        const {$db: _, ...queryWithoutDollarDb} = query;
        const explain = (() => {
            if (query.find) {
                return assert.commandWorked(this.db.runCommand({explain: queryWithoutDollarDb}));
            } else if (query.aggregate) {
                return assert.commandWorked(
                    this.db.runCommand({explain: {...queryWithoutDollarDb, cursor: {}}}));
            } else {
                return null;
            }
        })();
        if (explain) {
            getQueryPlanners(explain).forEach(queryPlanner => {
                assert.docEq(expectedQuerySettings, queryPlanner.querySettings, queryPlanner);
            });
        }
    }

    /**
     * Remove all query settings for the current tenant.
     */
    removeAllQuerySettings() {
        let settingsArray = this.getQuerySettings();
        while (settingsArray.length > 0) {
            const setting = settingsArray.pop();
            assert.commandWorked(
                this.adminDB.runCommand({removeQuerySettings: setting.representativeQuery}));
            this.assertQueryShapeConfiguration(settingsArray);
        }
    }
}
