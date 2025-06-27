// Tests writeConcern metrics in the serverStatus output for a replica set.
// @tags: [
//   requires_persistence,
//   requires_replication,
// ]
// Verifies that the server status response has the fields that we expect.
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    generateCmdsWithNoWCProvided,
    verifyServerStatusChange,
    verifyServerStatusFields,
} from "jstests/noPassthrough/libs/write_concern_metrics_helpers.js";

let rst;
let primary;
let secondary;
const dbName = "test";
const collName = "server_write_concern_metrics";
let testDB;
let testColl;

function initializeReplicaSet(isPSASet) {
    let replSetNodes = [{}, {}];
    if (isPSASet) {
        replSetNodes.push({arbiter: true});
    }
    rst = new ReplSetTest({
        nodes: replSetNodes,
        nodeOptions: {setParameter: 'reportOpWriteConcernCountersInServerStatus=true'}
    });
    rst.startSet();
    let config = rst.getReplSetConfig();
    config.members[1].priority = 0;
    config.members[0].tags = {dc_va: "rack1"};
    config.settings = {getLastErrorModes: {myTag: {dc_va: 1}}};
    rst.initiate(config);
    primary = rst.getPrimary();
    secondary = rst.getSecondary();
    testDB = primary.getDB(dbName);
    testColl = testDB[collName];
}

function resetCollection(setupCommand) {
    testColl.drop();
    assert.commandWorked(testDB.createCollection(collName));
    if (setupCommand) {
        assert.commandWorked(testDB.runCommand(setupCommand));
    }
}

function testWriteConcernMetrics(cmd, opName, inc, isPSASet, setupCommand) {
    jsTestLog("Testing " + opName + " - IsPSA: " + isPSASet);
    initializeReplicaSet(isPSASet);

    // Run command with no writeConcern and no CWWC set.
    const cmdsWithNoWCProvided = generateCmdsWithNoWCProvided(cmd);
    let serverStatus, newStatus;
    cmdsWithNoWCProvided.forEach(cmd => {
        resetCollection(setupCommand);
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                 newStatus.opWriteConcernCounters,
                                 [
                                     opName +
                                         (isPSASet ? ".noneInfo.implicitDefault.wnum.1"
                                                   : ".noneInfo.implicitDefault.wmajority"),
                                     opName + ".none"
                                 ],
                                 inc);
    });

    // Run command with no writeConcern with CWWC set to majority.
    assert.commandWorked(primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "majority"},
        writeConcern: {w: "majority"}
    }));
    cmdsWithNoWCProvided.forEach(cmd => {
        resetCollection(setupCommand);
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
    assert.commandWorked(primary.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    cmdsWithNoWCProvided.forEach(cmd => {
        resetCollection(setupCommand);
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
    assert.commandWorked(primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "myTag"},
        writeConcern: {w: "majority"}
    }));
    cmdsWithNoWCProvided.forEach(cmd => {
        resetCollection(setupCommand);
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
    assert.soon(() => {
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        try {
            verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                                     newStatus.opWriteConcernCounters,
                                     [opName + ".wnum.0"],
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

    rst.stopSet();
}

for (const isPSASet of [true, false]) {
    // Test single insert/update/delete.
    testWriteConcernMetrics({insert: collName, documents: [{}]}, "insert", 1, isPSASet);
    testWriteConcernMetrics(
        {update: collName, updates: [{q: {}, u: {$set: {a: 1}}}]}, "update", 1, isPSASet);
    testWriteConcernMetrics(
        {delete: collName, deletes: [{q: {}, limit: 1}]}, "delete", 1, isPSASet);

    // Test batch writes.
    testWriteConcernMetrics({insert: collName, documents: [{}, {}]}, "insert", 2, isPSASet);
    testWriteConcernMetrics(
        {update: collName, updates: [{q: {}, u: {$set: {a: 1}}}, {q: {}, u: {$set: {a: 1}}}]},
        "update",
        2,
        isPSASet);
    testWriteConcernMetrics(
        {delete: collName, deletes: [{q: {}, limit: 1}, {q: {}, limit: 1}]}, "delete", 2, isPSASet);

    // Test applyOps.  All sequences of setup + command must be idempotent in steady-state oplog
    // application, as testWriteConcernMetrics will run them multiple times.
    testWriteConcernMetrics(
        {applyOps: [{op: "i", ns: testColl.getFullName(), o: {_id: 0}}]}, "insert", 1, isPSASet);
    testWriteConcernMetrics({
        applyOps: [{
            op: "u",
            ns: testColl.getFullName(),
            o2: {_id: 0},
            o: {$v: 2, diff: {u: {a: 1}}},
            b: true
        }]
    },
                            "update",
                            1,
                            isPSASet);
    testWriteConcernMetrics({applyOps: [{op: "d", ns: testColl.getFullName(), o: {_id: 0}}]},
                            "delete",
                            1,
                            isPSASet,
                            {insert: collName, documents: [{_id: 0}]});
}
