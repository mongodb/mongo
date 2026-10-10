/**
 * Tests that database commands related to persisted query settings fail gracefully when the BSON
 * object size limit is exceeded.
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// A single index name large enough that two of them cannot both fit in the 16MB 'querySettings'
// cluster parameter, but one of them can.
const kLargeIndexName = "a".repeat(10 * 1024 * 1024);

function testSizeLimits(getDB) {
    const collName = "query_settings_size_limits";
    let db;
    let qsutils;
    let queryA;
    let queryB;
    let querySettingsWithSmallIndexName;
    let querySettingsWithLargeIndexName;

    beforeEach(function () {
        db = getDB();
        qsutils = new QuerySettingsUtils(db, collName);
        queryA = qsutils.makeFindQueryInstance({filter: {a: "a"}});
        queryB = qsutils.makeFindQueryInstance({filter: {b: "b"}});

        const ns = {db: db.getName(), coll: collName};
        querySettingsWithSmallIndexName = {indexHints: {ns, allowedIndexes: ["a"]}};
        querySettingsWithLargeIndexName = {indexHints: {ns, allowedIndexes: [kLargeIndexName]}};

        assertDropAndRecreateCollection(db, collName);
        qsutils.removeAllQuerySettings();
    });

    afterEach(function () {
        // Perform query settings cleanup.
        qsutils.removeAllQuerySettings();
    });

    // SPM-3684 will store representative queries in the 'queryShapeRepresentativeQueries'
    // collection, which makes 16MB limit of query settings harder to reach. Due to that, we will
    // specify query settings with large index names in order to reach the limit.
    it("should not contain a representative query if failed to set query settings", function () {
        // Specifying query settings with a single large index name should succeed, as the total
        // size of the 'querySettings' cluster parameter is less than 16MB.
        assert.commandWorked(
            db.adminCommand({setQuerySettings: queryA, settings: querySettingsWithLargeIndexName}),
        );

        // Due to orphaned representative queries, we can not run assertRepresentativeQueries() with
        // an empty array, so we capture the existing representative queries and ensure no new ones
        // are added.
        const existingRepresentativeQueries = qsutils.getRepresentativeQueries();

        // Specifying query settings with the same large index name should fail as total size of
        // 'querySettings' cluster parameter exceeds 16MB.
        assert.commandFailedWithCode(
            db.adminCommand({setQuerySettings: queryB, settings: querySettingsWithLargeIndexName}),
            ErrorCodes.BSONObjectTooLarge,
        );
        qsutils.assertRepresentativeQueries(existingRepresentativeQueries);

        // Ensure that only a single query setting is present.
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryA),
        ]);

        // Specifying query settings with a total size of less than 16MB should still work.
        assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: {reject: true}}));

        // Ensure that both query shape configurations are present.
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryA),
            qsutils.makeQueryShapeConfiguration({reject: true}, queryB),
        ]);
    });

    it("should contain a representative query if we successfully inserted a query settings, but then failed to update it due to 16MB limit", function () {
        // Set query settings with a 10MB index name, which should succeed and representative
        // query should be present.
        assert.commandWorked(
            db.adminCommand({setQuerySettings: queryA, settings: querySettingsWithSmallIndexName}),
        );
        assert.commandWorked(
            db.adminCommand({setQuerySettings: queryB, settings: querySettingsWithLargeIndexName}),
        );
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(querySettingsWithSmallIndexName, queryA),
            qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryB),
        ]);

        // Due to orphaned representative queries, we can not run assertRepresentativeQueries()
        // with an empty array, so we capture the existing representative queries and ensure no
        // new ones are added.
        const existingRepresentativeQueries = qsutils.getRepresentativeQueries();
        assert.commandFailedWithCode(
            db.adminCommand({setQuerySettings: queryA, settings: querySettingsWithLargeIndexName}),
            ErrorCodes.BSONObjectTooLarge,
        );
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(querySettingsWithSmallIndexName, queryA),
            qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryB),
        ]);
        qsutils.assertRepresentativeQueries(existingRepresentativeQueries);
    });
}

describe("QuerySettings size limits on a replica set", function () {
    let rst;

    before(function () {
        rst = new ReplSetTest({nodes: 1});
        rst.startSet();
        rst.initiate();
    });

    after(function () {
        rst.stopSet();
    });

    testSizeLimits(() => rst.getPrimary().getDB("test"));
});

describe("QuerySettings size limits on a sharded cluster", function () {
    let st;

    before(function () {
        st = new ShardingTest({shards: 1, mongos: 1});
    });

    after(function () {
        st.stop();
    });

    testSizeLimits(() => st.s.getDB("test"));
});
