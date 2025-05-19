/*
 * This test makes sure that change streams can be opened with different 'version' field values.
 * @tags: [
 *   # "version" parameter for change streams is only available from v8.2 onwards.
 *   requires_fcv_82
 * ]
 */
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

const dbName = jsTestName();
const collName = jsTestName();

const validVersions = ["v1", "v2", undefined];

function testChangeStreamWithVersionAttributeSet(version = undefined) {
    let changeStreamParams = {};
    if (version !== undefined) {
        changeStreamParams.version = version;
    }

    // Specifying the "version" field does nothing when opening a change stream on a replica set or
    // on a mongod, but it is still permitted to specify it.
    const tests = [
        {},                      // Whole-cluster change stream
        {collection: 1},         // Whole-DB change stream
        {collection: collName},  // Collection change stream
    ];

    tests.forEach((nss) => {
        db.getSiblingDB(dbName).dropDatabase();

        const isWholeClusterChangeStream = !nss.hasOwnProperty('collection');

        const testDB = db.getSiblingDB(dbName);
        const cst = new ChangeStreamTest(isWholeClusterChangeStream ? testDB.getSiblingDB("admin")
                                                                    : testDB);

        let cursor;
        try {
            cursor = (() => {
                if (isWholeClusterChangeStream) {
                    return cst.startWatchingAllChangesForCluster({}, changeStreamParams);
                }
                return cst.startWatchingChanges(
                    {pipeline: [{$changeStream: changeStreamParams}], collection: nss.collection});
            })();
            assert(cursor);
            assert(validVersions.includes(version),
                   `expecting change stream to succeed with version ${tojson(version)}`);

            assert.commandWorked(testDB.getCollection(collName).insert({_id: 1}));

            const expected = {
                documentKey: {_id: 1},
                fullDocument: {_id: 1},
                ns: {db: dbName, coll: collName},
                operationType: "insert",
            };

            cst.assertNextChangesEqual({cursor, expectedChanges: [expected]});
        } catch (err) {
            assert(!validVersions.includes(version),
                   `expecting change stream to fail with version ${tojson(version)}`);
        } finally {
            cst.cleanUp();
        }
    });
}

validVersions.forEach((version) => {
    testChangeStreamWithVersionAttributeSet(version);
});

testChangeStreamWithVersionAttributeSet("v3");  // fails
testChangeStreamWithVersionAttributeSet("v");   // fails
