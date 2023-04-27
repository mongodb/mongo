/**
 * This file tests that if a user initiates a bulk write where the last write is a noop, either
 * due to being a duplicate operation or due to an error based on data we read, that we
 * still wait for write concern.
 * The intended behavior for a no-op write is that we advance the repl client's last optime to the
 * optime of the newest entry in the oplog (also referred as the "system optime"), and wait for
 * write concern for that optime. This ensures that any writes we may have possibly read that caused
 * the operation to be a noop have also been replicated. For all of these tests, the optime fixing
 * behavior should be handled by LastOpFixer.
 *
 * @tags: [featureFlagBulkWriteCommand] // TODO SERVER-52419: Remove this tag.
 */

(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");
load("jstests/libs/parallel_shell_helpers.js");
load("jstests/libs/fail_point_util.js");

const name = jsTestName();
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
    nodeOptions: {
        setParameter: {
            // Prevent inserts from being batched together. This allows
            // us to hang between consecutive insert operations without
            // blocking the ones we already processed from executing.
            internalInsertMaxBatchSize: 1,
        }
    }
});
replTest.startSet();
replTest.initiate();

const primary = replTest.getPrimary();
const dbName = 'testDB';
const testDB = primary.getDB(dbName);
const collName = 'testColl';
const coll = testDB[collName];

function dropTestCollection() {
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");
}

// Each entry in this array contains a bulkWrite command noop write case we want to test.
// Entries have the following structure:
// {
//
//      bulkReq: <object>,              // Bulk write request object containing multiple writes
//                                       // where the last write will result in a noop
//                                       // write if it is run after noopMakerReq.
//
//     bulkConfirmFunc: <function(bulkRes)>,   // Function to run after bulkReq and to ensure
//                                             // it executed as expected. Accepts the result
//                                             // of the bulkWrite request.
//
//     noopMakerReq: <object>            // Command request object containing a single non-bulk
//                                       //  write that, if run before the final write in bulkReq,
//                                       // will make that write a noop.
//
//     noopMakerConfirmFunc: <function(noopMakerRes)>,  // Function to run after noopMakerReq to
//                                                      // ensure it executed as expected. Accepts
//                                                      // the result of the request.
//
//     confirmFunc: <function()>   // Function to run at the end of the test to make any general
//                                 // assertions that are not on either of the command responses.
// }
let commands = [];

// 'bulkWrite' where the last op is an insert where the document with the same _id has
// already been inserted.
commands.push({
    bulkReq: {
        bulkWrite: 1,
        ops: [{insert: 0, document: {_id: 0}}, {insert: 0, document: {_id: 1}}],
        nsInfo: [{ns: `${dbName}.${collName}`}]
    },
    bulkConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
        assert.eq(res.cursor.firstBatch.length, 2);

        // The first insert succeeded
        assert.eq(res.cursor.firstBatch[0].ok, 1);
        assert.eq(res.cursor.firstBatch[0].n, 1);

        // The second insert errored
        assert.eq(res.cursor.firstBatch[1].ok, 0);
        assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.DuplicateKey);
    },
    noopMakerReq: {insert: collName, documents: [{_id: 1}]},
    noopMakerConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
    },
    confirmFunc: function() {
        assert.eq(coll.count({_id: 0}), 1);
        assert.eq(coll.count({_id: 1}), 1);
    }
});

// 'bulkWrite' where we are doing a mix of local and non-local writes and the last op is an insert
// of a non-local doc with the _id of an existing doc.
const localDBName = "local";
const localDB = primary.getDB("local");
const localColl = localDB[collName];
localColl.drop();

commands.push({
    bulkReq: {
        bulkWrite: 1,
        ops: [{insert: 0, document: {_id: 1}}, {insert: 1, document: {_id: 1}}],
        nsInfo: [{ns: `${localDBName}.${collName}`}, {ns: `${dbName}.${collName}`}]
    },
    bulkConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
        assert.eq(res.cursor.firstBatch.length, 2);

        // the local insert succeeded
        assert.eq(res.cursor.firstBatch[0].ok, 1);
        assert.eq(res.cursor.firstBatch[0].n, 1);

        // the non-local insert failed
        assert.eq(res.cursor.firstBatch[1].ok, 0);
        assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.DuplicateKey);
    },
    noopMakerReq: {insert: collName, documents: [{_id: 1}]},
    noopMakerConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
    },
    confirmFunc: function(res) {
        assert.eq(coll.count({_id: 1}), 1);
        assert.eq(localColl.count({_id: 1}), 1);
    }
});

// 'bulkWrite' where the last op is an update that has already been performed.
commands.push({
    bulkReq: {
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {_id: 0}},
            {update: 0, filter: {_id: 0}, updateMods: {$set: {x: 1}}}
        ],
        nsInfo: [{ns: `${dbName}.${collName}`}]
    },
    noopMakerReq: {update: collName, updates: [{q: {_id: 0}, u: {$set: {x: 1}}}]},
    bulkConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.cursor.firstBatch.length, 2);

        // The insert succeeded.
        assert.eq(res.cursor.firstBatch[0].ok, 1);
        assert.eq(res.cursor.firstBatch[0].n, 1);

        // The update was a noop.
        assert.eq(res.cursor.firstBatch[1].ok, 1);
        assert.eq(res.cursor.firstBatch[1].n, 1);
        assert.eq(res.cursor.firstBatch[1].nModified, 0);
    },
    noopMakerConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
        assert.eq(res.nModified, 1);
    },
    confirmFunc: function() {
        assert.eq(coll.count({_id: 0, x: 1}), 1);
    }
});

// 'bulkWrite' where the last op is an update where the document to update does not exist.
commands.push({
    bulkReq: {
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {a: 1}},
            {update: 0, filter: {a: 1}, updateMods: {$set: {x: 1}}}
        ],
        nsInfo: [{ns: `${dbName}.${collName}`}],
    },
    bulkConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.cursor.firstBatch.length, 2);

        // the insert succeeded
        assert.eq(res.cursor.firstBatch[0].ok, 1);
        assert.eq(res.cursor.firstBatch[0].n, 1);

        // the update was a no-op
        assert.eq(res.cursor.firstBatch[1].ok, 1);
        assert.eq(res.cursor.firstBatch[1].n, 0);
        assert.eq(res.cursor.firstBatch[1].nModified, 0);
    },
    noopMakerReq: {update: collName, updates: [{q: {a: 1}, u: {b: 2}}]},
    noopMakerConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
        assert.eq(res.nModified, 1);
    },
    confirmFunc: function() {
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({b: 2}), 1);
    }
});

// 'bulkWrite' where the last op is an update that generates an immutable field error.
commands.push({
    bulkReq: {
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {_id: 0}},
            {update: 0, filter: {_id: 1}, updateMods: {$set: {_id: 2}}}
        ],
        nsInfo: [{ns: `${dbName}.${collName}`}],
    },
    bulkConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
        assert.eq(res.cursor.firstBatch.length, 2);

        // the insert succeeded
        assert.eq(res.cursor.firstBatch[0].ok, 1);
        assert.eq(res.cursor.firstBatch[0].n, 1);

        // the update failed
        assert.eq(res.cursor.firstBatch[1].ok, 0);
        assert.eq(res.cursor.firstBatch[1].code, ErrorCodes.ImmutableField);
        assert.eq(res.cursor.firstBatch[1].n, 0);
        assert.eq(res.cursor.firstBatch[1].nModified, 0);
    },
    noopMakerReq: {insert: collName, documents: [{_id: 1}]},
    noopMakerConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
    },
    confirmFunc: function() {
        assert.eq(coll.count({_id: 0}), 1);
        assert.eq(coll.count({_id: 1}), 1);
    }
});

// 'bulkWrite' where the last op is a delete where the document to delete does not exist.
commands.push({
    bulkReq: {
        bulkWrite: 1,
        ops: [{insert: 0, document: {x: 1}}, {delete: 0, filter: {x: 1}, multi: false}],
        nsInfo: [{ns: `${dbName}.${collName}`}],
    },
    bulkConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.cursor.firstBatch.length, 2);

        // the insert op succeeded
        var res1 = res.cursor.firstBatch[0];
        assert.eq(res1.ok, 1);
        assert.eq(res1.n, 1);

        // the delete was a no-op
        var res2 = res.cursor.firstBatch[1];
        assert.eq(res2.ok, 1);
        assert.eq(res2.n, 0);
    },
    noopMakerReq: {delete: collName, deletes: [{q: {x: 1}, limit: 1}]},
    noopMakerConfirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
    },
    confirmFunc: function(res) {
        assert.eq(coll.count({x: 1}), 0);
    }
});

function testCommandWithWriteConcern(cmd) {
    // Provide a small wtimeout that we expect to time out.
    cmd.bulkReq.writeConcern = {w: 3, wtimeout: 1000};
    jsTest.log("Testing " + tojson(cmd.bulkReq));

    dropTestCollection();

    let failpoint = configureFailPoint(testDB, 'hangBetweenProcessingBulkWriteOps', {}, {skip: 1});

    function runBulkReq(host, cmd) {
        load('jstests/libs/write_concern_util.js');

        // Tests that the command receives a write concern error. If we don't properly advance
        // the client's last optime to the latest oplog entry and wait for that optime to
        // satisfy our write concern, then we won't see an error, since all writes up to but not
        // not including the latest one in `noopMakerReq` have been replicated.

        // Since we run this on a separate connection from the noopMakerReq, there is no way
        // that the client's last op time would get advanced by that operation, so if we pass
        // this test it means we are correctly advancing this client's optime after the last
        // operation in the batch no-ops.
        const res = new Mongo(host).getDB('admin').runCommand(cmd.bulkReq);
        try {
            assertWriteConcernError(res);
            cmd.bulkConfirmFunc(res);
        } catch (e) {
            // Make sure that we print out the response.
            printjson(res);
            throw e;
        }
    }

    // Run in a parallel shell as we expect this to hang.
    const awaitBulkWrite =
        startParallelShell(funWithArgs(runBulkReq, primary.host, cmd), replTest.ports[0]);

    // Wait to see that the bulkWrite has hit the failpoint.
    failpoint.wait();

    // Wait until all of the nodes have seen the first write from the bulkWrite.
    replTest.awaitReplication();

    // Stop a node so that all w:3 write concerns time out.
    replTest.stop(1);

    // Run the function that makes the final bulk write op a no-op.
    // Provide a small wtimeout that we expect to time out.
    cmd.noopMakerReq.writeConcern = {w: 3, wtimeout: 1000};
    var noopMakerRes = testDB.runCommand(cmd.noopMakerReq);
    cmd.noopMakerConfirmFunc(noopMakerRes);

    // Disable the failpoint, allowing the bulkWrite to proceed.
    failpoint.off();

    awaitBulkWrite();
    cmd.confirmFunc();

    replTest.start(1);
}

commands.forEach(function(cmd) {
    testCommandWithWriteConcern(cmd);
});

replTest.stopSet();
})();
