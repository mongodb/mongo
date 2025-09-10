/*
 * This test makes sure that change streams can be opened with different 'version' field values.
 * @tags: [
 *   # "version" parameter for change streams is only available from v8.2 onwards.
 *   requires_fcv_82,
 *   uses_change_streams,
 * ]
 */
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";

const dbName = jsTestName();
const collName = jsTestName();

// TODO SERVER-106575 / SERVER-107442: reenable testing of "v2" reader version here.
const validVersions = ["v1", undefined];
const isPreciseShardTargetingEnabled = FeatureFlagUtil.isEnabled(db, "ChangeStreamPreciseShardTargeting");

function testChangeStreamWithVersionAttributeSet(version = undefined) {
    // Specifying the "version" field does nothing when opening a change stream on a replica set or
    // on a mongod, but it is still permitted to specify it.
    let changeStreamParams = {};
    if (version !== undefined) {
        changeStreamParams.version = version;
    }

    let tests = [
        {collection: collName}, // Collection change stream
    ];

    // The change stream reader version of 'v2' can be requested even if the feature flag for precise shard
    // targeting is disabled. In this case the change stream reader version will silently fall back to 'v1'.
    // If the feature flag is enabled, we currently only support collection-level change streams for v2 readers.
    // Database-level and all database-change streams are currently not implemented for v2 readers, so we
    // only add them to the test when it is safe to do so (non-v2 change stream reader and/or feature flag is
    // disabled).
    if (version !== "v2" || !isPreciseShardTargetingEnabled) {
        tests = tests.concat([
            {collection: 1}, // Whole-DB change stream
            {}, // Whole-cluster change stream
        ]);
    }

    tests.forEach((nss) => {
        db.getSiblingDB(dbName).dropDatabase();

        const isWholeClusterChangeStream = !nss.hasOwnProperty("collection");

        const testDB = db.getSiblingDB(dbName);
        const cst = new ChangeStreamTest(isWholeClusterChangeStream ? testDB.getSiblingDB("admin") : testDB);

        let cursor;
        try {
            cursor = (() => {
                if (isWholeClusterChangeStream) {
                    return cst.startWatchingAllChangesForCluster({}, changeStreamParams);
                }
                return cst.startWatchingChanges({
                    pipeline: [{$changeStream: changeStreamParams}],
                    collection: nss.collection,
                });
            })();
            assert(cursor);
            assert(
                validVersions.includes(version),
                `expecting successful change stream for version ${tojson(version)} to be included in validVersions`,
            );

            assert.commandWorked(testDB.getCollection(collName).insert({_id: 1}));

            const expected = {
                documentKey: {_id: 1},
                fullDocument: {_id: 1},
                ns: {db: dbName, coll: collName},
                operationType: "insert",
            };

            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        } catch (err) {
            assert(
                !validVersions.includes(version),
                `expecting unsuccessful change stream for version ${tojson(
                    version,
                )} to be excluded from validVersions. Error: ${tojsononeline(err)}`,
            );
        } finally {
            cst.cleanUp();
        }
    });
}

validVersions.forEach((version) => {
    testChangeStreamWithVersionAttributeSet(version);
});

testChangeStreamWithVersionAttributeSet("v3"); // fails
testChangeStreamWithVersionAttributeSet("v"); // fails
