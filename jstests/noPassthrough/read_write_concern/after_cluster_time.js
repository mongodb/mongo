// This test verifies readConcern:afterClusterTime behavior on a standalone mongod.
// @tags: [requires_replication, requires_majority_read_concern]
import {ReplSetTest} from "jstests/libs/replsettest.js";

let standalone = MongoRunner.runMongod({storageEngine: "wiredTiger"});

let testDB = standalone.getDB("test");

assert.commandWorked(testDB.runCommand({insert: "after_cluster_time", documents: [{x: 1}]}));

// Majority reads without afterClusterTime succeed.
assert.commandWorked(
    testDB.runCommand({find: "after_cluster_time", readConcern: {level: "majority"}}),
    "expected majority read without afterClusterTime to succeed on standalone mongod",
);

// afterClusterTime reads without a level fail.
assert.commandFailedWithCode(
    testDB.runCommand({find: "after_cluster_time", readConcern: {afterClusterTime: Timestamp(0, 0)}}),
    ErrorCodes.InvalidOptions,
    "expected non-majority afterClusterTime read to fail on standalone mongod",
);

// afterClusterTime reads with null timestamps are rejected.
assert.commandFailedWithCode(
    testDB.runCommand({
        find: "after_cluster_time",
        readConcern: {level: "majority", afterClusterTime: Timestamp(0, 0)},
    }),
    ErrorCodes.InvalidOptions,
    "expected afterClusterTime read with null timestamp to fail on standalone mongod",
);

// Standalones don't support any operations with clusterTime.
assert.commandFailedWithCode(
    testDB.runCommand({
        find: "after_cluster_time",
        readConcern: {level: "majority", afterClusterTime: Timestamp(0, 1)},
    }),
    ErrorCodes.IllegalOperation,
    "expected afterClusterTime read to fail on standalone mongod",
);
MongoRunner.stopMongod(standalone);

let rst = new ReplSetTest({nodes: 1});
rst.startSet();
rst.initiate();
let adminDBRS = rst.getPrimary().getDB("admin");

let res = adminDBRS.runCommand({ping: 1});
assert.commandWorked(res);
assert(res.hasOwnProperty("$clusterTime"), tojson(res));
assert(res.$clusterTime.hasOwnProperty("clusterTime"), tojson(res));
let clusterTime = res.$clusterTime.clusterTime;
// afterClusterTime is not allowed in  ping command.
assert.commandFailedWithCode(
    adminDBRS.runCommand({ping: 1, readConcern: {afterClusterTime: clusterTime}}),
    ErrorCodes.InvalidOptions,
    "expected afterClusterTime fail in ping",
);

// afterClusterTime is not allowed in serverStatus command.
assert.commandFailedWithCode(
    adminDBRS.runCommand({serverStatus: 1, readConcern: {afterClusterTime: clusterTime}}),
    ErrorCodes.InvalidOptions,
    "expected afterClusterTime fail in serverStatus",
);

// afterClusterTime is not allowed in currentOp command.
assert.commandFailedWithCode(
    adminDBRS.runCommand({currentOp: 1, readConcern: {afterClusterTime: clusterTime}}),
    ErrorCodes.InvalidOptions,
    "expected afterClusterTime fail in serverStatus",
);

rst.stopSet();
