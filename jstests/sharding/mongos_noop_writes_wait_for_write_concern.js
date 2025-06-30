// Sharding version of jstests/replsets/noop_writes_wait_for_write_concern.js.
// @tags: [
//  multiversion_incompatible
//  ]

import {getNoopWriteCommands} from "jstests/libs/noop_write_commands.js";
import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

// Create a shard with 3 nodes and stop one of the secondaries. This will allow majority write
// concern to be met, but w: 3 will always time out.
const st = new ShardingTest({mongos: 1, shards: 1, rs: {nodes: 3}});
const secondary = st.rs0.getSecondary();
st.rs0.stop(secondary);

const mongos = st.s;
const dbName = 'testDB';
const db = mongos.getDB(dbName);
const collName = 'testColl';
const coll = db[collName];
const commands = getNoopWriteCommands(coll, "sharding");

commands.forEach((cmd) => {
    if ("applyOps" in cmd.req) {
        // applyOps is not available through mongos.
        return;
    }

    if ("dropDatabase" in cmd.req || "drop" in cmd.req) {
        // TODO SERVER-80103: dropDatabase and dropCollection do not respect user supplied write
        // concern and instead always use majority.
        return;
    }

    if ("create" in cmd.req) {
        // TODO SERVER-80103: create returns WriteConcernFailed as an ordinary error code instead of
        // using the writeConcernError field.
        return;
    }

    // We run the command on a different connection. If the the command were run on the
    // same connection, then the client last op for the noop write would be set by the setup
    // operation. By using a fresh connection the client last op begins as null.
    // This test explicitly tests that write concern for noop writes works when the
    // client last op has not already been set by a duplicate operation.
    const shell = new Mongo(mongos.host);
    cmd.run(dbName, coll, shell);
});

// Restart the node so that consistency checks performed by st.stop() can succeed.
st.rs0.restart(secondary);
st.stop();
