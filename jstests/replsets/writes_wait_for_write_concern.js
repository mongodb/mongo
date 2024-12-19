/**
 * This file tests that if a user initiates a write that becomes a noop either due to being a
 * duplicate operation or due to errors relying on data reads, that we still wait for write concern.
 * This is because we must wait for write concern on the write that made this a noop so that we can
 * be sure it doesn't get rolled back if we acknowledge it.
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {getWriteCommands} from "jstests/libs/write_commands.js";
import {assertWriteConcernError} from "jstests/libs/write_concern_util.js";

const name = jsTestName();
const replTest = new ReplSetTest({
    name: name,
    nodes: [{}, {rsConfig: {priority: 0}}, {rsConfig: {priority: 0}}],
});
replTest.startSet();
replTest.initiate();
// Stops node 1 so that all w:3 write concerns time out. We have 3 data bearing nodes so that
// 'dropDatabase' can satisfy its implicit writeConcern: majority but still time out from the
// explicit w:3 write concern.
replTest.stop(1);

const primary = replTest.getPrimary();
assert.eq(primary, replTest.nodes[0]);
const dbName = 'testDB';
const db = primary.getDB(dbName);
const collName = 'testColl';
const coll = db[collName];
const commands = getWriteCommands(coll, "rs");

commands.forEach(function(cmd) {
    // Don't run drop cmd on older versions. Starting in v7.0 drop returns OK on non-existent
    // collections, instead of a NamespaceNotFound error.
    if (cmd.req["drop"] !== undefined) {
        const primaryConn = new Mongo(primary.host);
        const primaryBinVersion = primaryConn.getDB("admin").serverStatus()["version"];
        if (primaryBinVersion != MongoRunner.getBinVersionFor("latest")) {
            jsTest.log(
                "Skipping test: drop on non-existent collections in older versions returns an error.");
            return;
        }
        if (primaryBinVersion == MongoRunner.getBinVersionFor("latest")) {
            // TODO SERVER-97754 Remove this condition that disables the test case for "drop".
            jsTest.log("Skipping drop test: no WCE in replica-set, SERVER-97754.");
            return;
        }
    }

    if (cmd.req["create"] !== undefined) {
        // TODO SERVER-97754 Remove this condition that disables the test case for "create".
        jsTest.log("Skipping create test: no WCE in replica-set, SERVER-97754.");
        return;
    }

    // We run the command on a different connection. If the the command were run on the
    // same connection, then the client last op for the noop write would be set by the setup
    // operation. By using a fresh connection the client last op begins as null.
    // This test explicitly tests that write concern for noop writes works when the
    // client last op has not already been set by a duplicate operation.
    const conn = new Mongo(primary.host);
    cmd.run(coll, conn);
});

replTest.stopSet();
