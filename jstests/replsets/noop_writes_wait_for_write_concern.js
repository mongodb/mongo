/**
 * This file tests that if a user initiates a write that becomes a noop either due to being a
 * duplicate operation or due to errors relying on data reads, that we still wait for write concern.
 * This is because we must wait for write concern on the write that made this a noop so that we can
 * be sure it doesn't get rolled back if we acknowledge it.
 */

(function() {
"use strict";
load('jstests/libs/write_concern_util.js');
load('jstests/noPassthrough/libs/index_build.js');

var name = 'noop_writes_wait_for_write_concern';
var replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
});
replTest.startSet();
replTest.initiate();
// Stops node 1 so that all w:3 write concerns time out. We have 3 data bearing nodes so that
// 'dropDatabase' can satisfy its implicit writeConcern: majority but still time out from the
// explicit w:3 write concern.
replTest.stop(1);

var primary = replTest.getPrimary();
assert.eq(primary, replTest.nodes[0]);
var dbName = 'testDB';
var db = primary.getDB(dbName);
var collName = 'testColl';
var coll = db[collName];

function dropTestCollection() {
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");
}

// Each entry in this array contains a command whose noop write concern behavior needs to be
// tested. Entries have the following structure:
// {
//      req: <object>,                   // Command request object that will result in a noop
//                                       // write after the setup function is called.
//
//      setupFunc: <function()>,         // Function to run to ensure that the request is a
//                                       // noop.
//
//      confirmFunc: <function(res)>,    // Function to run after the command is run to ensure
//                                       // that it executed properly. Accepts the result of
//                                       // the noop request to validate it.
// }
var commands = [];

// 'applyOps' where the update has already been done.
commands.push({
    req: {applyOps: [{op: "u", ns: coll.getFullName(), o: {_id: 1}, o2: {_id: 1}}]},
    setupFunc: function() {
        assert.commandWorked(coll.insert({_id: 1}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.applied, 1);
        assert.eq(res.results[0], true);
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({_id: 1}), 1);
    }
});

// 'update' where the document to update does not exist.
commands.push({
    req: {update: collName, updates: [{q: {a: 1}, u: {b: 2}}]},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.update({a: 1}, {b: 2}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 0);
        assert.eq(res.nModified, 0);
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({b: 2}), 1);
    }
});

// 'update' where the update has already been done.
commands.push({
    req: {update: collName, updates: [{q: {a: 1}, u: {$set: {b: 2}}}]},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.update({a: 1}, {$set: {b: 2}}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 1);
        assert.eq(res.nModified, 0);
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({a: 1, b: 2}), 1);
    }
});

// 'update' with immutable field error.
commands.push({
    req: {update: collName, updates: [{q: {_id: 1}, u: {$set: {_id: 2}}}]},
    setupFunc: function() {
        assert.commandWorked(coll.insert({_id: 1}));
    },
    confirmFunc: function(res) {
        assert.eq(res.n, 0);
        assert.eq(res.nModified, 0);
        assert.eq(coll.count({_id: 1}), 1);
    }
});

// 'delete' where the document to delete does not exist.
commands.push({
    req: {delete: collName, deletes: [{q: {a: 1}, limit: 1}]},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.remove({a: 1}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.n, 0);
        assert.eq(coll.count({a: 1}), 0);
    }
});

// 'createIndexes' where the index has already been created.
// All voting data bearing nodes are not up for this test. So 'createIndexes' command can't succeed
// with the default index commitQuorum value "votingMembers". So, running createIndexes cmd using
// commit quorum "majority".
commands.push({
    req: {createIndexes: collName, indexes: [{key: {a: 1}, name: "a_1"}], commitQuorum: "majority"},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: "a_1"}],
            commitQuorum: "majority"
        }));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.numIndexesBefore, res.numIndexesAfter);
        assert.eq(res.note, 'all indexes already exist');
    }
});

// 'findAndModify' where the document to update does not exist.
commands.push({
    req: {findAndModify: collName, query: {a: 1}, update: {b: 2}},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorkedIgnoringWriteConcernErrors(
            db.runCommand({findAndModify: collName, query: {a: 1}, update: {b: 2}}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.lastErrorObject.updatedExisting, false);
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({b: 2}), 1);
    }
});

// 'findAndModify' where the update has already been done.
commands.push({
    req: {findAndModify: collName, query: {a: 1}, update: {$set: {b: 2}}},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorkedIgnoringWriteConcernErrors(
            db.runCommand({findAndModify: collName, query: {a: 1}, update: {$set: {b: 2}}}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.lastErrorObject.updatedExisting, true);
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({a: 1, b: 2}), 1);
    }
});

// 'findAndModify' with immutable field error.
commands.push({
    req: {findAndModify: collName, query: {_id: 1}, update: {$set: {_id: 2}}},
    setupFunc: function() {
        assert.commandWorked(coll.insert({_id: 1}));
    },
    confirmFunc: function(res) {
        assert.commandFailedWithCode(res, ErrorCodes.ImmutableField);
        assert.eq(coll.find().itcount(), 1);
        assert.eq(coll.count({_id: 1}), 1);
    }
});

// 'findAndModify' where the document to delete does not exist.
commands.push({
    req: {findAndModify: collName, query: {a: 1}, remove: true},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorked(coll.remove({a: 1}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
        assert.eq(res.lastErrorObject.n, 0);
    }
});

// 'dropDatabase' where the database has already been dropped.
commands.push({
    req: {dropDatabase: 1},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({dropDatabase: 1}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
    }
});

// 'drop' where the collection has already been dropped.
commands.push({
    req: {drop: collName},
    setupFunc: function() {
        assert.commandWorked(coll.insert({a: 1}));
        assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({drop: collName}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteConcernErrors(res);
    }
});

// 'create' where the collection has already been created.
commands.push({
    req: {create: collName},
    setupFunc: function() {
        assert.commandWorkedIgnoringWriteConcernErrors(db.runCommand({create: collName}));
    },
    confirmFunc: function(res) {
        // Branching is needed for multiversion tests as 'create' is only idempotent as of 7.0.
        // TODO SERVER-74062: update this to stop branching on the server version and always
        // assert the command worked ignoring write concern errors.
        if (db.version().split('.')[0] >= 7) {
            assert.commandWorkedIgnoringWriteConcernErrors(res);
        } else {
            assert.commandFailedWithCode(res, ErrorCodes.NamespaceExists);
        }
    }
});

// 'insert' where the document with the same _id has already been inserted.
commands.push({
    req: {insert: collName, documents: [{_id: 1}]},
    setupFunc: function() {
        assert.commandWorked(coll.insert({_id: 1}));
    },
    confirmFunc: function(res) {
        assert.commandWorkedIgnoringWriteErrorsAndWriteConcernErrors(res);
        assert.eq(res.n, 0);
        assert.eq(res.writeErrors[0].code, ErrorCodes.DuplicateKey);
        assert.eq(coll.count({_id: 1}), 1);
    }
});

function testCommandWithWriteConcern(cmd) {
    // Provide a small wtimeout that we expect to time out.
    cmd.req.writeConcern = {w: 3, wtimeout: 1000};
    jsTest.log("Testing " + tojson(cmd.req));

    // Don't run drop cmd on older versions. Starting in v7.0 drop returns OK on non-existent
    // collections, instead of a NamespaceNotFound error.
    if (cmd.req["drop"] !== undefined) {
        const primaryShell = new Mongo(primary.host);
        const primaryBinVersion = primaryShell.getDB("admin").serverStatus()["version"];
        if (primaryBinVersion != MongoRunner.getBinVersionFor("latest")) {
            jsTest.log(
                "Skipping test: drop on non-existent collections in older versions returns an error.");
            return;
        }
    }

    dropTestCollection();

    cmd.setupFunc();

    // We run the command on a different connection. If the the command were run on the
    // same connection, then the client last op for the noop write would be set by the setup
    // operation. By using a fresh connection the client last op begins as null.
    // This test explicitly tests that write concern for noop writes works when the
    // client last op has not already been set by a duplicate operation.
    var shell2 = new Mongo(primary.host);

    // We check the error code of 'res' in the 'confirmFunc'.
    var res = "bulkWrite" in cmd.req ? shell2.adminCommand(cmd.req)
                                     : shell2.getDB(dbName).runCommand(cmd.req);

    try {
        // Tests that the command receives a write concern error. If we don't wait for write
        // concern on noop writes then we won't get a write concern error.
        assertWriteConcernError(res);
        cmd.confirmFunc(res);
    } catch (e) {
        // Make sure that we print out the response.
        printjson(res);
        throw e;
    }
}

commands.forEach(function(cmd) {
    testCommandWithWriteConcern(cmd);
});

replTest.stopSet();
})();
