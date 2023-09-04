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
     * Makes an query instance of the find command with an optional filter clause.
     */
    makeQueryInstance(filter = {}) {
        return {find: this.collName, $db: this.db.getName(), filter};
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
    getQuerySettings() {
        return this.adminDB
            .aggregate([
                {$querySettings: {}},
                {$project: {queryShapeHash: 0}},
                {$sort: {representativeQuery: 1}},
            ])
            .toArray();
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
            assert.docEq(expectedQuerySettings,
                         getQueryPlanner(explain).querySettings,
                         explain.queryPlanner);
        }
    }

    // Adjust the 'clusterServerParameterRefreshIntervalSecs' value for faster fetching of
    // 'querySettings' cluster parameter on mongos from the configsvr.
    setClusterParamRefreshSecs(newValue) {
        if (FixtureHelpers.isMongos(this.db)) {
            const response = assert.commandWorked(this.db.adminCommand(
                {getParameter: 1, clusterServerParameterRefreshIntervalSecs: 1}));
            const oldValue = response.clusterServerParameterRefreshIntervalSecs;
            assert.commandWorked(this.db.adminCommand(
                {setParameter: 1, clusterServerParameterRefreshIntervalSecs: newValue}));
            return {
                restore: () => {
                    assert.commandWorked(this.db.adminCommand(
                        {setParameter: 1, clusterServerParameterRefreshIntervalSecs: oldValue}));
                }
            };
        }
        return {restore: () => {}};
    }

    /**
     * Remove all query settings for the current tenant.
     */
    removeAllQuerySettings() {
        this.adminDB.aggregate([{$querySettings: {}}])
            .toArray()
            .forEach(el => assert.commandWorked(
                         this.adminDB.runCommand({removeQuerySettings: el.queryShapeHash})),
                     this);
        this.assertQueryShapeConfiguration([]);
    }
}
