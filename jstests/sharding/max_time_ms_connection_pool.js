/**
 *
 * Tests the rewrite of NetworkInterfaceExceededTimeLimit exception coming from
 * `executor/connection_pool.cpp` into MaxTimeMSError when MaxTimeMS option is set for a given
 * sharding command.
 *
 * @tags: [requires_fcv_60, __TEMPORARILY_DISABLED__]
 */

(function() {
"use strict";

load("jstests/libs/fail_point_util.js");

const databaseName = "my-database";
const collectionName = "my-collection";

function generateInsertCommand(maxTimeMS) {
    return {insert: collectionName, documents: [{}], maxTimeMS: maxTimeMS};
}

const st = new ShardingTest({shards: 1, mongos: 1});
const database = st.s0.getDB(databaseName);
const collection = database.getCollection(collectionName);
const session = database.getMongo().startSession({causalConsistency: false});

assert.commandWorked(
    database.runCommand({create: collection.getName(), writeConcern: {w: "majority"}}));

assert.commandWorked(database.runCommand(generateInsertCommand(1000)));

const failpoint =
    configureFailPoint(st.s, "forceExecutorConnectionPoolTimeout", {"timeout": 1000}, "alwaysOn");

assert.commandFailedWithCode(database.runCommand(generateInsertCommand(1)),
                             ErrorCodes.MaxTimeMSExpired);

assert.commandFailedWithCode(database.runCommand(generateInsertCommand(30000)),
                             ErrorCodes.NetworkInterfaceExceededTimeLimit);

failpoint.off();

st.stop();
}());
