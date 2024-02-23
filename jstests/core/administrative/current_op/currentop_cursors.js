/**
 * Tests that an idle cursor will appear in the $currentOp output if the idleCursors option is
 * set to true.
 *
 * @tags: [
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   requires_capped,
 *   no_selinux,
 *   # This test contains assertions for the hostname that operations run on.
 *   tenant_migration_incompatible,
 *   docker_incompatible,
 *   grpc_incompatible,
 * ]
 */
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const coll = db.jstests_currentop_cursors;

// Avoiding using the shell helper to avoid the implicit collection recreation.
db.runCommand({drop: coll.getName()});
assert.commandWorked(db.createCollection(coll.getName(), {capped: true, size: 1000}));
for (let i = 0; i < 30; ++i) {
    assert.commandWorked(coll.insert({"val": i}));
}
/**
 * runTest creates a new collection called jstests_currentop_cursors and then runs the provided
 * find query. It calls $currentOp and does some basic assertions to make sure idleCursors is
 * behaving as intended in each case.
 * findFunc: A function that runs a find query. Is expected to return a cursorID.
 *  Arbitrary code can be run in findFunc as long as it returns a cursorID.
 * assertFunc: A function that runs assertions against the results of the $currentOp.
 * Takes the following arguments
 *  'findOut': The cursorID returned from findFunc.
 *  'result': The results from running $currenpOp as an array of JSON objects.
 * Arbitrary code can be run in assertFunc, and there is no return value needed.
 */
function runTest({findFunc, assertFunc}) {
    const adminDB = db.getSiblingDB("admin");
    const findOut = findFunc();
    const result = adminDB
                       .aggregate([
                           {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                           {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                       ])
                       .toArray();
    assert.eq(result[0].ns, coll.getFullName(), result);
    assert.eq(result[0].cursor.originatingCommand.find, coll.getName(), result);
    assertFunc(findOut, result);
    const noIdle = adminDB
                       .aggregate([
                           {$currentOp: {allUsers: false, idleCursors: false}},
                           {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": findOut}]}}
                       ])
                       .toArray();
    assert.eq(noIdle.length, 0, tojson(noIdle));
    const noFlag =
        adminDB.aggregate([{$currentOp: {allUsers: false}}, {$match: {type: "idleCursor"}}])
            .toArray();

    assert.eq(noIdle.length, 0, tojson(noFlag));
}

// Basic test with default values.
runTest({
    findFunc: function() {
        return assert
            .commandWorked(db.runCommand({find: "jstests_currentop_cursors", batchSize: 2}))
            .cursor.id;
    },
    assertFunc: function(cursorId, result) {
        assert.eq(result.length, 1, result);
        // Plan summary does not exist on mongos, so skip this test on mongos.
        // TODO (SERVER-86780): idleCursor currentOp doc on sharded cluster doesn't have
        // "planSummary".
        if (!FixtureHelpers.isMongos(db) && !TestData.testingReplicaSetEndpoint) {
            assert.eq(result[0].planSummary, "COLLSCAN", result);
        } else {
            assert(!result[0].hasOwnProperty("planSummary"), result);
        }
        assert(result[0].lsid.hasOwnProperty('id'), result);
        assert(result[0].lsid.hasOwnProperty('uid'), result);
        const uri = new MongoURI(db.getMongo().host);
        assert(uri.servers.some((server) => {
            return result[0].host == getHostName() + ":" + server.port;
        }));
        const idleCursor = result[0].cursor;
        assert.eq(idleCursor.nDocsReturned, 2, result);
        assert.eq(idleCursor.nBatchesReturned, 1, result);
        assert.eq(idleCursor.tailable, false, result);
        assert.eq(idleCursor.awaitData, false, result);
        assert.eq(idleCursor.noCursorTimeout, false, result);
        assert.eq(idleCursor.originatingCommand.batchSize, 2, result);
        assert.lte(idleCursor.createdDate, idleCursor.lastAccessDate, result);
        // Make sure that the top level fields do not also appear in the cursor subobject.
        assert(!idleCursor.hasOwnProperty("planSummary"), result);
        assert(!idleCursor.hasOwnProperty('host'), result);
        assert(!idleCursor.hasOwnProperty('lsid'), result);
    }
});

// Test that tailable, awaitData, and noCursorTimeout are set.
runTest({
    findFunc: function() {
        return assert
            .commandWorked(db.runCommand({
                find: "jstests_currentop_cursors",
                batchSize: 2,
                tailable: true,
                awaitData: true,
                noCursorTimeout: true
            }))
            .cursor.id;
    },
    assertFunc: function(cursorId, result) {
        assert.eq(result.length, 1, result);
        const idleCursor = result[0].cursor;
        assert.eq(idleCursor.tailable, true, result);
        assert.eq(idleCursor.awaitData, true, result);
        assert.eq(idleCursor.noCursorTimeout, true, result);
        assert.eq(idleCursor.originatingCommand.batchSize, 2, result);
    }
});

// Test that dates are set correctly.
runTest({
    findFunc: function() {
        return assert
            .commandWorked(db.runCommand({find: "jstests_currentop_cursors", batchSize: 2}))
            .cursor.id;
    },
    assertFunc: function(cursorId, result) {
        const adminDB = db.getSiblingDB("admin");
        // Make sure the two cursors have different creation times.
        assert.soon(() => {
            const secondCursor = assert.commandWorked(
                db.runCommand({find: "jstests_currentop_cursors", batchSize: 2}));

            const secondResult =
                adminDB
                    .aggregate([
                        {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                        {
                            $match: {
                                $and: [
                                    {type: "idleCursor"},
                                    {"cursor.cursorId": secondCursor.cursor.id}
                                ]
                            }
                        }
                    ])
                    .toArray();
            return result[0].cursor.createdDate < secondResult[0].cursor.createdDate;
        });
    }
});

// Test larger batch size.
runTest({
    findFunc: function() {
        return assert
            .commandWorked(db.runCommand(
                {find: "jstests_currentop_cursors", batchSize: 4, noCursorTimeout: true}))
            .cursor.id;
    },
    assertFunc: function(cursorId, result) {
        const idleCursor = result[0].cursor;
        assert.eq(result.length, 1, result);
        assert.eq(idleCursor.nDocsReturned, 4, result);
        assert.eq(idleCursor.nBatchesReturned, 1, result);
        assert.eq(idleCursor.noCursorTimeout, true, result);
        assert.eq(idleCursor.originatingCommand.batchSize, 4, result);
    }
});

// Test batchSize and nDocs are incremented correctly.
runTest({
    findFunc: function() {
        return assert
            .commandWorked(db.runCommand({find: "jstests_currentop_cursors", batchSize: 2}))
            .cursor.id;
    },
    assertFunc: function(cursorId, result) {
        const adminDB = db.getSiblingDB("admin");
        const originalAccess = result[0].cursor.lastAccessDate;
        assert.commandWorked(db.runCommand(
            {getMore: cursorId, collection: "jstests_currentop_cursors", batchSize: 2}));
        result = adminDB
                     .aggregate([
                         {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                         {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                     ])
                     .toArray();
        let idleCursor = result[0].cursor;
        assert.eq(idleCursor.nDocsReturned, 4, result);
        assert.eq(idleCursor.nBatchesReturned, 2, result);
        assert.eq(idleCursor.originatingCommand.batchSize, 2, result);
        // Make sure that the getMore will not finish running in the same milli as the cursor
        // creation.
        assert.soon(() => {
            assert.commandWorked(db.runCommand(
                {getMore: cursorId, collection: "jstests_currentop_cursors", batchSize: 2}));
            result = adminDB
                         .aggregate([
                             {$currentOp: {localOps: true, allUsers: false, idleCursors: true}},
                             {$match: {$and: [{type: "idleCursor"}, {"cursor.cursorId": cursorId}]}}
                         ])
                         .toArray();
            idleCursor = result[0].cursor;
            return idleCursor.createdDate < idleCursor.lastAccessDate &&
                originalAccess < idleCursor.lastAccessDate;
        });
    }
});

// planSummary does not exist on Mongos, so skip this test.
// TODO (SERVER-86780): idleCursor currentOp doc on sharded cluster doesn't have "planSummary".
if (!FixtureHelpers.isMongos(db) && !TestData.testingReplicaSetEndpoint) {
    runTest({
        findFunc: function() {
            assert.commandWorked(coll.createIndex({"val": 1}));
            return assert
                .commandWorked(db.runCommand(
                    {find: "jstests_currentop_cursors", filter: {"val": {$gt: 2}}, batchSize: 2}))
                .cursor.id;
        },
        assertFunc: function(cursorId, result) {
            assert.eq(result.length, 1, result);
            assert.eq(result[0].planSummary, "IXSCAN { val: 1 }", result);
        }
    });
}
// Test lsid.id value is correct.
const session = db.getMongo().startSession();
runTest({
    findFunc: function() {
        const sessionDB = session.getDatabase("test");
        return assert
            .commandWorked(sessionDB.runCommand({find: "jstests_currentop_cursors", batchSize: 2}))
            .cursor.id;
    },
    assertFunc: function(cursorId, result) {
        assert.eq(result.length, 1, result);
        assert.eq(session.getSessionId().id, result[0].lsid.id);
    }
});
