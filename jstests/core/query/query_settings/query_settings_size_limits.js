/**
 * Tests that database commands related to persisted query settings fail gracefully when BSON object
 * size limit is exceeded.
 * @tags: [
 *   # TODO SERVER-98659 Investigate why this test is failing on
 *   # 'sharding_kill_stepdown_terminate_jscore_passthrough'.
 *   does_not_support_stepdowns,
 *   directly_against_shardsvrs_incompatible,
 *   requires_non_retryable_commands,
 *   simulate_atlas_proxy_incompatible,
 *   requires_fcv_80,
 *   # TODO SERVER-89461 Investigate why test using huge batch size timeout in suites with balancer.
 *   assumes_balancer_off,
 * ]
 */
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {afterEach, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

const dbName = db.getName();
const collName = jsTestName();
const ns = {
    db: dbName,
    coll: collName
};

describe("QuerySettings", function() {
    const qsutils = new QuerySettingsUtils(db, collName);
    const queryA = qsutils.makeFindQueryInstance({filter: {a: "a"}});
    const queryB = qsutils.makeFindQueryInstance({filter: {b: "b"}});
    const querySettingsWithSmallIndexName = {indexHints: {ns, allowedIndexes: ["a"]}};
    const querySettingsWithLargeIndexName = {
        indexHints: {ns, allowedIndexes: ["a".repeat(10 * 1024 * 1024)]}
    };

    beforeEach(function() {
        assertDropAndRecreateCollection(db, collName);
        qsutils.removeAllQuerySettings();
    });

    afterEach(function() {
        // Perform query settings cleanup.
        qsutils.removeAllQuerySettings();
    });

    // SPM-3684 will store representative queries in the 'queryShapeRepresentativeQueries'
    // collection, which makes 16MB limit of query settings harder to reach. Due to that, we will
    // specify query settings with large index names in order to reach the limit.
    it("should not contain a representative query if failed to set query settings", function() {
        // Specifying query settings with the same large index name should succed as total size of
        // 'querySettings' cluster parameter is less than 16MB.
        assert.commandWorked(
            db.adminCommand({setQuerySettings: queryA, settings: querySettingsWithLargeIndexName}));

        // Due to orphaned representative queries, we can not run assertRepresentativeQueries() with
        // an empty array, so we capture the existing representative queries and ensure no new ones
        // are added.
        const existingRepresentativeQueries = qsutils.getRepresentativeQueries();

        // Specifying query settings with the same large index name should fail as total size of
        // 'querySettings' cluster parameter exceeds 16MB.
        assert.commandFailedWithCode(
            db.adminCommand({setQuerySettings: queryB, settings: querySettingsWithLargeIndexName}),
            ErrorCodes.BSONObjectTooLarge);
        qsutils.assertRepresentativeQueries(existingRepresentativeQueries);

        // Ensure that only a single query settings is present.
        qsutils.assertQueryShapeConfiguration(
            [qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryA)]);

        // Specifying query settings with total size less than 16MB should still work.
        assert.commandWorked(db.adminCommand({setQuerySettings: queryB, settings: {reject: true}}));

        // Ensure that both query shape configurations are present.
        qsutils.assertQueryShapeConfiguration([
            qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryA),
            qsutils.makeQueryShapeConfiguration({reject: true}, queryB)
        ]);
    });

    it("should contain a representative query if we successfully inserted a query settings, but then failed to update it due to 16MB limit",
       function() {
           // Set query settings with a 10MB index name, which should succeed and representative
           // query should be present.
           assert.commandWorked(db.adminCommand(
               {setQuerySettings: queryA, settings: querySettingsWithSmallIndexName}));
           assert.commandWorked(db.adminCommand(
               {setQuerySettings: queryB, settings: querySettingsWithLargeIndexName}));
           qsutils.assertQueryShapeConfiguration([
               qsutils.makeQueryShapeConfiguration(querySettingsWithSmallIndexName, queryA),
               qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryB),
           ]);

           // Due to orphaned representative queries, we can not run assertRepresentativeQueries()
           // with an empty array, so we capture the existing representative queries and ensure no
           // new ones are added.
           const existingRepresentativeQueries = qsutils.getRepresentativeQueries();
           assert.commandFailedWithCode(
               db.adminCommand(
                   {setQuerySettings: queryA, settings: querySettingsWithLargeIndexName}),
               ErrorCodes.BSONObjectTooLarge);
           qsutils.assertQueryShapeConfiguration([
               qsutils.makeQueryShapeConfiguration(querySettingsWithSmallIndexName, queryA),
               qsutils.makeQueryShapeConfiguration(querySettingsWithLargeIndexName, queryB),
           ]);
           qsutils.assertRepresentativeQueries(existingRepresentativeQueries);
       });
});
