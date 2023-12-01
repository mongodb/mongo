/**
 * Utility class for testing query settings.
 */
import {getQueryPlanner} from "jstests/libs/analyze_plan.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

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
     * Makes an query instance of the aggregate command with an optional pipeline clause.
     */
    makeAggregateQueryInstance(pipeline = [], collName = this.collName) {
        return {aggregate: collName, $db: this.db.getName(), pipeline};
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
     * Return the query settings section of the server status.
     */
    getQuerySettingsServerStatus() {
        return this.db.runCommand({serverStatus: 1}).querySettings;
    }

    /**
     * Helper function to assert equality of QueryShapeConfigurations. In order to ease the
     * assertion logic, 'queryShapeHash' field is removed from the QueryShapeConfiguration prior to
     * assertion.
     *
     * Since in sharded clusters the query settings may arrive with a delay to the mongos, the
     * assertion is done via 'assert.soon'.
     */
    assertQueryShapeConfiguration(expectedQueryShapeConfigurations) {
        assert.soon(
            () => {
                return bsonWoCompare(this.getQuerySettings(), expectedQueryShapeConfigurations) ==
                    0;
            },
            "current query settings = " + tojson(this.getQuerySettings()) +
                ", expected query settings = " + tojson(expectedQueryShapeConfigurations));

        for (let {representativeQuery, settings} of expectedQueryShapeConfigurations) {
            this.assertExplainQuerySettings(representativeQuery, settings);
        }
    }

    /**
     * Asserts that the explain output for 'query' contains 'expectedQuerySettings'.
     */
    assertExplainQuerySettings(query, expectedQuerySettings) {
        // Pass query without the $db field to explain command, because it injects the $db field
        // inside the query before processing.
        const {$db: _, ...queryWithoutDollarDb} = query;
        if (query.find) {
            const explain =
                assert.commandWorked(this.db.runCommand({explain: queryWithoutDollarDb}));
            const queryPlanner = getQueryPlanner(explain);
            assert.docEq(expectedQuerySettings, queryPlanner.querySettings, queryPlanner);
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
