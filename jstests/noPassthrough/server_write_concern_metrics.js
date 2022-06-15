// Tests writeConcern metrics in the serverStatus output.
// @tags: [
//   requires_persistence,
//   requires_replication,
// ]
(function() {
"use strict";

// Verifies that the server status response has the fields that we expect.
function verifyServerStatusFields(serverStatusResponse) {
    assert(serverStatusResponse.hasOwnProperty("opWriteConcernCounters"),
           "Expected the serverStatus response to have a 'opWriteConcernCounters' field\n" +
               tojson(serverStatusResponse));
    assert(serverStatusResponse.opWriteConcernCounters.hasOwnProperty("insert"),
           "The 'opWriteConcernCounters' field in serverStatus did not have the 'insert' field\n" +
               tojson(serverStatusResponse.opWriteConcernCounters));
    assert(serverStatusResponse.opWriteConcernCounters.hasOwnProperty("update"),
           "The 'opWriteConcernCounters' field in serverStatus did not have the 'update' field\n" +
               tojson(serverStatusResponse.opWriteConcernCounters));
    assert(serverStatusResponse.opWriteConcernCounters.hasOwnProperty("delete"),
           "The 'opWriteConcernCounters' field in serverStatus did not have the 'delete' field\n" +
               tojson(serverStatusResponse.opWriteConcernCounters));
}

// Verifies that the given path of the server status response is incremented in the way we
// expect, and no other changes occurred. This function modifies its inputs.
function verifyServerStatusChange(initialStats, newStats, paths, expectedIncrement) {
    paths.forEach(path => {
        // Traverse to the parent of the changed element.
        let pathComponents = path.split(".");
        let initialParent = initialStats;
        let newParent = newStats;
        for (let i = 0; i < pathComponents.length - 1; i++) {
            assert(initialParent.hasOwnProperty(pathComponents[i]),
                   "initialStats did not contain component " + i + " of path " + path +
                       ", initialStats: " + tojson(initialStats));
            initialParent = initialParent[pathComponents[i]];

            assert(newParent.hasOwnProperty(pathComponents[i]),
                   "newStats did not contain component " + i + " of path " + path +
                       ", newStats: " + tojson(newStats));
            newParent = newParent[pathComponents[i]];
        }

        // Test the expected increment of the changed element. The element may not exist in the
        // initial stats, in which case it is treated as 0.
        let lastPathComponent = pathComponents[pathComponents.length - 1];
        let initialValue = 0;
        if (initialParent.hasOwnProperty(lastPathComponent)) {
            initialValue = initialParent[lastPathComponent];
        }
        assert(newParent.hasOwnProperty(lastPathComponent),
               "newStats did not contain last component of path " + path +
                   ", newStats: " + tojson(newStats));
        assert.eq(initialValue + expectedIncrement,
                  newParent[lastPathComponent],
                  "expected " + path + " to increase by " + expectedIncrement + ", initialStats: " +
                      tojson(initialStats) + ", newStats: " + tojson(newStats));

        // Delete the changed element.
        delete initialParent[lastPathComponent];
        delete newParent[lastPathComponent];
    });

    // The stats objects should be equal without the changed element.
    assert.eq(0,
              bsonWoCompare(initialStats, newStats),
              "expected initialStats and newStats to be equal after removing " + tojson(paths) +
                  ", initialStats: " + tojson(initialStats) + ", newStats: " + tojson(newStats));
}

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
    initializeReplicaSet(isPSASet);

    // Run command with no writeConcern and no CWWC set.
    resetCollection(setupCommand);
    let serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    let newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [
                                 opName +
                                     (isPSASet ? ".noneInfo.implicitDefault.wnum.1"
                                               : ".noneInfo.implicitDefault.wmajority"),
                                 opName + ".none"
                             ],
                             inc);

    // Run command with no writeConcern with CWWC set to majority.
    resetCollection(setupCommand);
    assert.commandWorked(primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "majority"},
        writeConcern: {w: "majority"}
    }));
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".noneInfo.CWWC.wmajority", opName + ".none"],
                             inc);

    // Run command with no writeConcern with CWWC set to w:1.
    resetCollection(setupCommand);
    assert.commandWorked(primary.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".noneInfo.CWWC.wnum.1", opName + ".none"],
                             inc);

    // Run command with no writeConcern and with CWWC set to j:true.
    resetCollection(setupCommand);
    assert.commandWorked(primary.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {j: true}, writeConcern: {w: "majority"}}));
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".noneInfo.CWWC.wnum.1", opName + ".none"],
                             inc);

    // Run command with no writeConcern and with CWWC set with (w: "myTag").
    resetCollection(setupCommand);
    assert.commandWorked(primary.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "myTag"},
        writeConcern: {w: "majority"}
    }));
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".noneInfo.CWWC.wtag.myTag", opName + ".none"],
                             inc);

    // Run command with writeConcern {j: true}. This should be counted as having no 'w' value.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {j: true}})));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".noneInfo.implicitDefault.wnum.1", opName + ".none"],
                             inc);

    // Run command with writeConcern {w: "majority"}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: "majority"}})));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wmajority"],
                             inc);

    // Run command with writeConcern {w: 0}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 0}})));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wnum.0"],
                             inc);

    // Run command with writeConcern {w: 1}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 1}})));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wnum.1"],
                             inc);

    // Run command with writeConcern {w: 2}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 2}})));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(serverStatus.opWriteConcernCounters,
                             newStatus.opWriteConcernCounters,
                             [opName + ".wnum.2"],
                             inc);

    // Run command with writeConcern {w: "myTag"}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(
        testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: "myTag"}})));
    newStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
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
        applyOps:
            [{op: "u", ns: testColl.getFullName(), o2: {_id: 0}, o: {$v: 2, diff: {u: {a: 1}}}}]
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
}());
