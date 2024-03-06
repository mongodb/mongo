// Tests writeConcern metrics in the serverStatus output for sharded cluster.
// @tags: [
//   requires_persistence,
//   requires_replication,
// ]

import {
    generateCmdsWithNoWCProvided,
    verifyServerStatusChange,
    verifyServerStatusFields,
} from "jstests/noPassthrough/server_write_concern_metrics.js";

let rst;
let conn;
let st;
let primary;
let secondary;
const dbName = "test";
const collName = "server_write_concern_metrics";
let testDB;
let testColl;

function initializeCluster() {
    rst = new ReplSetTest({
        nodes: [{}, {}],
        nodeOptions: {shardsvr: "", setParameter: 'reportOpWriteConcernCountersInServerStatus=true'}
    });
    rst.startSet();
    let config = rst.getReplSetConfig();
    config.members[1].priority = 0;
    config.members[0].tags = {dc_va: "rack1"};
    config.settings = {getLastErrorModes: {myTag: {dc_va: 1}}};
    rst.initiate(config);
    primary = rst.getPrimary();
    secondary = rst.getSecondary();
    st = new ShardingTest({manualAddShard: true});
    assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
    conn = st.s;
    testDB = conn.getDB(dbName);
    testColl = testDB[collName];
}

function resetCollection(setupCommand) {
    testColl.drop();
    assert.commandWorked(testDB.createCollection(collName));
    // The create coordinator issues a best effort refresh at the end of the coordinator which can
    // inferfere with the counts in the test cases. Wait here for the refreshes to finish.
    let curOps = [];
    assert.soon(() => {
        curOps = primary.getDB("admin")
                     .aggregate([
                         {$currentOp: {allUsers: true}},
                         {$match: {"command._flushRoutingTableCacheUpdates": {$exists: true}}}
                     ])
                     .toArray();
        return curOps.length == 0;
    }, "Timed out waiting for create refreshes to finish, found: " + tojson(curOps));
    if (setupCommand) {
        assert.commandWorked(testDB.runCommand(setupCommand));
    }
}

function testWriteConcernMetrics(cmd, opName, inc, setupCommand) {
    jsTestLog("Testing " + opName);
    initializeCluster();

    // Run command with no writeConcern and no CWWC set.
    resetCollection(setupCommand);
    const cmdsWithNoWCProvided = generateCmdsWithNoWCProvided(cmd);
    let serverStatus, newStatus;
    cmdsWithNoWCProvided.forEach(cmd => {
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                 newStatus.opWriteConcernCounters,
                                 [opName + ".noneInfo.implicitDefault.wmajority", opName + ".none"],
                                 inc);
    });

    // Run command with no writeConcern with CWWC set to majority.
    resetCollection(setupCommand);
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "majority"},
        writeConcern: {w: "majority"}
    }));
    cmdsWithNoWCProvided.forEach(cmd => {
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                 newStatus.opWriteConcernCounters,
                                 [opName + ".noneInfo.CWWC.wmajority", opName + ".none"],
                                 inc);
    });

    // Run command with no writeConcern with CWWC set to w:1.
    resetCollection(setupCommand);
    assert.commandWorked(conn.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    cmdsWithNoWCProvided.forEach(cmd => {
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                 newStatus.opWriteConcernCounters,
                                 [opName + ".noneInfo.CWWC.wnum.1", opName + ".none"],
                                 inc);
    });

    // Run command with no writeConcern and with CWWC set with (w: "myTag").
    resetCollection(setupCommand);
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "myTag"},
        writeConcern: {w: "majority"}
    }));
    cmdsWithNoWCProvided.forEach(cmd => {
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                 newStatus.opWriteConcernCounters,
                                 [opName + ".noneInfo.CWWC.wtag.myTag", opName + ".none"],
                                 inc);
    });

    // Run command with writeConcern {w: "majority"}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: "majority"}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wmajority"],
                             inc);

    // Run command with writeConcern {w: 0}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 0}})));
    // Because 'w:0' doesn't wait for an ack, the command might return before it got executed, hence
    // retrying.
    assert.soon(() => {
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        try {
            verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                     newStatus.opWriteConcernCounters,
                                     // Mongos upgrade the writeConcern when it is w:0.
                                     [opName + ".wnum.1"],
                                     inc);
        } catch (e) {
            return false;
        }
        return true;
    });

    // Run command with writeConcern {w: 1}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 1}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wnum.1"],
                             inc);

    // Run command with writeConcern {w: 2}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 2}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wnum.2"],
                             inc);

    // Run command with writeConcern {w: "myTag"}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: "myTag"}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wtag.myTag"],
                             inc);

    // writeConcern metrics are not tracked on the secondary.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    newStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));
    assert.eq(0,
              bsonWoCompare(serverStatus.opWriteConcernCounters, newStatus.opWriteConcernCounters),
              "expected no change in secondary writeConcern metrics, before: " +
                  tojson(serverStatus) + ", after: " + tojson(newStatus));

    st.stop();
    rst.stopSet();
}

// Test single insert/update/delete.
testWriteConcernMetrics({insert: collName, documents: [{}]}, "insert", 1);
testWriteConcernMetrics({update: collName, updates: [{q: {}, u: {$set: {a: 1}}}]}, "update", 1);
testWriteConcernMetrics({delete: collName, deletes: [{q: {}, limit: 1}]}, "delete", 1);

// Test batch writes.
testWriteConcernMetrics({insert: collName, documents: [{}, {}]}, "insert", 2);
testWriteConcernMetrics(
    {update: collName, updates: [{q: {}, u: {$set: {a: 1}}}, {q: {}, u: {$set: {a: 1}}}]},
    "update",
    2);
testWriteConcernMetrics(
    {delete: collName, deletes: [{q: {}, limit: 1}, {q: {}, limit: 1}]}, "delete", 2);
