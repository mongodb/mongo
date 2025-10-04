// Tests writeConcern metrics in the serverStatus output for sharded cluster.
// @tags: [
//   requires_persistence,
//   requires_replication,
// ]

import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    generateCmdsWithNoWCProvided,
    verifyServerStatusChange,
    verifyServerStatusFields,
} from "jstests/noPassthrough/libs/write_concern_metrics_helpers.js";
import {reconfig} from "jstests/replsets/rslib.js";

let conn;
let st;
let primary;
let secondary;
const dbName = "test";
const collName = "server_write_concern_metrics";
let testDB;
let testColl;

function initializeCluster() {
    st = new ShardingTest({
        name: "custom_write_concern_test",
        shards: {
            rs0: {
                nodes: [{rsConfig: {tags: {dc_va: "rack1"}}}, {rsConfig: {priority: 0}}],
                settings: {getLastErrorModes: {myTag: {dc_va: 1}}},
                setParameter: {
                    // Required for serverStatus() to have opWriteConcernCounters.
                    reportOpWriteConcernCountersInServerStatus: true,
                },
            },
        },
    });
    let cfg = st.configRS.getReplSetConfigFromNode();
    for (let i = 0; i < cfg.members.length; i++) {
        cfg.members[i].tags = {dc_va: "rack1"};
    }
    cfg.settings.getLastErrorModes = {myTag: {dc_va: 1}};
    reconfig(st.configRS, cfg);
    primary = st.rs0.getPrimary();
    secondary = st.rs0.getSecondary();
    conn = st.s;
    testDB = conn.getDB(dbName);
    testColl = testDB[collName];
}

function resetCollection(setupCommand) {
    testColl.drop();
    assert.commandWorked(testDB.createCollection(collName));
    // The collection creation does not include the refresh of its filtering metadata, which occurs
    // upon the first data access on the namespace and can inferfere with the counters checked in
    // the test cases (due to writes onto config.cache.collections/chunks).By performing a secomdary
    // read here, we'll ensure that subsequent requests will see stable filtering metadata.
    assert.commandWorked(
        testDB.runCommand({count: collName, $readPreference: {mode: "secondary"}, readConcern: {"level": "local"}}),
    );

    if (setupCommand) {
        assert.commandWorked(testDB.runCommand(setupCommand));
    }
}

function testWriteConcernMetrics(cmd, opName, inc, setupCommand) {
    jsTestLog("Testing " + opName);
    initializeCluster();

    // Run command with no writeConcern and no CWWC set.
    const cmdsWithNoWCProvided = generateCmdsWithNoWCProvided(cmd);
    let serverStatus, newStatus;
    cmdsWithNoWCProvided.forEach((cmd) => {
        resetCollection(setupCommand);
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(
            serverStatus.opWriteConcernCounters,
            newStatus.opWriteConcernCounters,
            [opName + ".noneInfo.implicitDefault.wmajority", opName + ".none"],
            inc,
        );
    });

    // Run command with no writeConcern with CWWC set to majority.
    assert.commandWorked(
        conn.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: "majority"},
            writeConcern: {w: "majority"},
        }),
    );
    cmdsWithNoWCProvided.forEach((cmd) => {
        resetCollection(setupCommand);
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(
            serverStatus.opWriteConcernCounters,
            newStatus.opWriteConcernCounters,
            [opName + ".noneInfo.CWWC.wmajority", opName + ".none"],
            inc,
        );
    });

    // Run command with no writeConcern with CWWC set to w:1.
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}),
    );
    cmdsWithNoWCProvided.forEach((cmd) => {
        resetCollection(setupCommand);
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(
            serverStatus.opWriteConcernCounters,
            newStatus.opWriteConcernCounters,
            [opName + ".noneInfo.CWWC.wnum.1", opName + ".none"],
            inc,
        );
    });

    // Run command with no writeConcern and with CWWC set with (w: "myTag").
    assert.commandWorked(
        conn.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: "myTag"},
            writeConcern: {w: "majority"},
        }),
    );
    cmdsWithNoWCProvided.forEach((cmd) => {
        resetCollection(setupCommand);
        serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusFields(serverStatus);
        assert.commandWorked(testDB.runCommand(cmd));
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        verifyServerStatusChange(
            serverStatus.opWriteConcernCounters,
            newStatus.opWriteConcernCounters,
            [opName + ".noneInfo.CWWC.wtag.myTag", opName + ".none"],
            inc,
        );
    });

    // Run command with writeConcern {w: "majority"}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: "majority"}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        serverStatus.opWriteConcernCounters,
        newStatus.opWriteConcernCounters,
        [opName + ".wmajority"],
        inc,
    );

    // Run command with writeConcern {w: 0}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 0}})));
    // Because 'w:0' doesn't wait for an ack, the command might return before it got
    // executed, hence retrying.
    assert.soon(() => {
        newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
        try {
            verifyServerStatusChange(
                serverStatus.opWriteConcernCounters,
                newStatus.opWriteConcernCounters,
                // Mongos upgrade the writeConcern when it is w:0.
                [opName + ".wnum.1"],
                inc,
            );
        } catch (e) {
            return false;
        }
        return true;
    });

    // Run command with writeConcern {w: 1}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 1}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        serverStatus.opWriteConcernCounters,
        newStatus.opWriteConcernCounters,
        [opName + ".wnum.1"],
        inc,
    );

    // Run command with writeConcern {w: 2}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: 2}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        serverStatus.opWriteConcernCounters,
        newStatus.opWriteConcernCounters,
        [opName + ".wnum.2"],
        inc,
    );

    // Run command with writeConcern {w: "myTag"}.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(Object.assign(Object.assign({}, cmd), {writeConcern: {w: "myTag"}})));
    newStatus = assert.commandWorked(primary.adminCommand({serverStatus: 1}));
    verifyServerStatusChange(
        serverStatus.opWriteConcernCounters,
        newStatus.opWriteConcernCounters,
        [opName + ".wtag.myTag"],
        inc,
    );

    // writeConcern metrics are not tracked on the secondary.
    resetCollection(setupCommand);
    serverStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));
    verifyServerStatusFields(serverStatus);
    assert.commandWorked(testDB.runCommand(cmd));
    newStatus = assert.commandWorked(secondary.adminCommand({serverStatus: 1}));
    assert.eq(
        0,
        bsonWoCompare(serverStatus.opWriteConcernCounters, newStatus.opWriteConcernCounters),
        "expected no change in secondary writeConcern metrics, before: " +
            tojson(serverStatus) +
            ", after: " +
            tojson(newStatus),
    );

    st.stop();
}

// Test single insert/update/delete.
testWriteConcernMetrics({insert: collName, documents: [{}]}, "insert", 1);
testWriteConcernMetrics({update: collName, updates: [{q: {}, u: {$set: {a: 1}}}]}, "update", 1);
testWriteConcernMetrics({delete: collName, deletes: [{q: {}, limit: 1}]}, "delete", 1);

// Test batch writes.
testWriteConcernMetrics({insert: collName, documents: [{}, {}]}, "insert", 2);
testWriteConcernMetrics(
    {
        update: collName,
        updates: [
            {q: {}, u: {$set: {a: 1}}},
            {q: {}, u: {$set: {a: 1}}},
        ],
    },
    "update",
    2,
);
testWriteConcernMetrics(
    {
        delete: collName,
        deletes: [
            {q: {}, limit: 1},
            {q: {}, limit: 1},
        ],
    },
    "delete",
    2,
);
