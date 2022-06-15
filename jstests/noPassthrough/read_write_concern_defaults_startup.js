// Verifies read/write concern defaults are loaded at startup by a mongos and non-sharded replica
// set node.
//
// This test restarts a replica set node, which requires persistence and journaling.
// @tags: [
//   requires_persistence,
//   requires_replication,
//   requires_sharding,
// ]
(function() {
"use strict";
load("jstests/replsets/rslib.js");  // For reconnect.

function runTest(conn, failPointConn, restartFn) {
    // Set a default rwc.
    assert.commandWorked(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}));
    let res = assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1}));
    assert.eq(res.defaultReadConcern, {level: "local"});

    // Restart the node with disabled rwc refreshes.
    restartFn();
    reconnect(conn);
    reconnect(failPointConn);

    // Verify the server attempted to load the defaults at startup.
    checkLog.contains(conn, "Error loading read and write concern defaults at startup");
    checkLog.contains(conn,
                      "Failing read/write concern persisted defaults lookup because of fail point");

    // Disable the fail point and verify the defaults can be returned.
    assert.commandWorked(
        failPointConn.adminCommand({configureFailPoint: "failRWCDefaultsLookup", mode: "off"}));
    res = assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1}));
    assert.eq(res.defaultReadConcern, {level: "local"});

    // Unset the default read concern so it won't interfere with testing hooks.
    assert.commandWorked(conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {}}));
}

jsTestLog("Testing mongos...");
{
    // Use 1 node CSRS so the failpoint to disable rwc lookup only needs to be set on one node.
    const st = new ShardingTest({shards: 1, config: 1});

    runTest(st.s, st.configRS.getPrimary(), () => {
        assert.commandWorked(st.configRS.getPrimary().adminCommand(
            {configureFailPoint: "failRWCDefaultsLookup", mode: "alwaysOn"}));
        st.restartMongos(0);
    });

    st.stop();
}

jsTestLog("Testing plain replica set node...");
{
    const rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    runTest(primary, primary, () => {
        rst.restart(
            primary,
            {setParameter: "failpoint.failRWCDefaultsLookup=" + tojson({mode: "alwaysOn"})});
    });

    rst.stopSet();
}
})();
