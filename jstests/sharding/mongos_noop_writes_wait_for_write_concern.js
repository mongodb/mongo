// Sharding version of jstests/replsets/noop_writes_wait_for_write_concern.js.
// @tags: [
//  multiversion_incompatible
//  ]

import {getNoopWriteCommands} from "jstests/libs/noop_write_commands.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

// Create a shard with 3 nodes and stop one of the secondaries. This will allow majority write
// concern to be met, but w: 3 will always time out.
var st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 3}});
const secondary = st.rs0.getSecondary();
st.rs0.stop(secondary);

const mongos = st.s;
var dbName = 'testDB';
var db = mongos.getDB(dbName);
var collName = 'testColl';
var coll = db[collName];
const commands = getNoopWriteCommands(coll);

function dropTestCollection() {
    coll.drop();
    assert.eq(0, coll.find().itcount(), "test collection not empty");
}

function testCommandWithWriteConcern(cmd) {
    if ("applyOps" in cmd.req) {
        // applyOps is not available through mongos.
        return;
    }

    if ("findAndModify" in cmd.req) {
        // TODO SERVER-80103: findAndModify does not return write concern errors in the presence of
        // other errors.
        return;
    }

    if ("dropDatabase" in cmd.req || "drop" in cmd.req) {
        // TODO SERVER-80103: dropDatabase and dropCollection do not respect user supplied write
        // concern and instead always use majority.
        return;
    }

    if ("create" in cmd.req) {
        // TODO SERVER-80103: create returns WriteConcernTimeout as an ordinary error code instead
        // of using the writeConcernError field.
        return;
    }

    // Provide a small wtimeout that we expect to time out.
    cmd.req.writeConcern = {w: 3, wtimeout: 1000};
    jsTest.log("Testing command: " + tojson(cmd.req));

    dropTestCollection();

    cmd.setupFunc();

    // We run the command on a different connection. If the the command were run on the
    // same connection, then the client last op for the noop write would be set by the setup
    // operation. By using a fresh connection the client last op begins as null.
    // This test explicitly tests that write concern for noop writes works when the
    // client last op has not already been set by a duplicate operation.
    const shell2 = new Mongo(mongos.host);

    // We check the error code of 'res' in the 'confirmFunc'.
    const res = "bulkWrite" in cmd.req ? shell2.adminCommand(cmd.req)
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

// Restart the node so that consistency checks performed by st.stop() can succeed.
st.rs0.restart(secondary);
st.stop();
