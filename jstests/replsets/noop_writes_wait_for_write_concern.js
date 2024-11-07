/**
 * This file tests that if a user initiates a write that becomes a noop due to being a duplicate
 * operation, that we still wait for write concern. This is because we must wait for write concern
 * on the write that made this a noop so that we can be sure it doesn't get rolled back if we
 * acknowledge it.
 */

(function() {
"use strict";
load('jstests/libs/write_concern_util.js');
load('jstests/libs/noop_write_commands.js');

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
const commands = getNoopWriteCommands(coll);

function dropTestCollection() {
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");
}

function testCommandWithWriteConcern(cmd) {
    // Provide a small wtimeout that we expect to time out.
    cmd.req.writeConcern = {w: 3, wtimeout: 1000};
    jsTest.log("Testing " + tojson(cmd.req));

    dropTestCollection();

    cmd.setupFunc();

    // We run the command on a different connection. If the the command were run on the
    // same connection, then the client last op for the noop write would be set by the setup
    // operation. By using a fresh connection the client last op begins as null.
    // This test explicitly tests that write concern for noop writes works when the
    // client last op has not already been set by a duplicate operation.
    var shell2 = new Mongo(primary.host);

    // We check the error code of 'res' in the 'confirmFunc'.
    var res = shell2.getDB(dbName).runCommand(cmd.req);

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
