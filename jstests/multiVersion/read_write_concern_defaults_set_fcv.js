/**
 * Tests that the default read write concern commands work as expected with
 * setFeatureCompatibilityVersion.
 */
(function() {
"use strict";

// Asserts the given defaults are eventually reflected on the given connections through
// getDefaultRWConcern with both inMemory true and false.
function assertDefaults(checkConns, {readConcern, writeConcern}) {
    for (let inMemory of [true, false]) {
        assert.soonNoExcept(
            () => {
                checkConns.forEach(conn => {
                    const res =
                        assert.commandWorked(conn.adminCommand({getDefaultRWConcern: 1, inMemory}));
                    assert.eq(res.defaultReadConcern,
                              readConcern,
                              tojson(res) + ", conn: " + tojson(conn));
                    assert.eq(res.defaultWriteConcern,
                              writeConcern,
                              tojson(res) + ", conn: " + tojson(conn));
                });
                return true;
            },
            "rwc defaults failed to propagate to all nodes, checkConns: " + tojson(checkConns) +
                ", readConcern: " + tojson(readConcern) + ", writeConcern: " + tojson(writeConcern),
            undefined,
            2000 /* interval */,
            {runHangAnalyzer: false});
    }
}

// Verifies the default read write concern is cleared on FCV downgrade and that the rwc commands are
// correctly gated by FCV.
function runTest(conn, checkConns, checkFCVConn) {
    checkFCV(checkFCVConn.getDB("admin"), latestFCV);

    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "majority"},
        defaultWriteConcern: {w: 1}
    }));
    assertDefaults(checkConns,
                   {readConcern: {level: "majority"}, writeConcern: {w: 1, wtimeout: 0}});

    jsTestLog("Downgrading FCV to last stable");
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(checkFCVConn.getDB("admin"), lastStableFCV);

    // The defaults document should have been cleared.
    assert.eq(null, conn.getDB("config").settings.findOne({_id: "ReadWriteConcernDefaults"}));

    // It should still be possible to run getDefaultRWConcern, but the defaults should have been
    // cleared.
    assertDefaults(checkConns, {readConcern: null, writeConcern: null});

    // Running setDefaultRWConcern should not be allowed.
    assert.commandFailedWithCode(
        conn.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}),
        ErrorCodes.CommandNotSupported);

    jsTestLog("Upgrading FCV to latest");
    assert.commandWorked(conn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(checkFCVConn.getDB("admin"), latestFCV);

    // There should still be no defaults document.
    assert.eq(null, conn.getDB("config").settings.findOne({_id: "ReadWriteConcernDefaults"}));

    // It should still be possible to run getDefaultRWConcern, and there should still be no defaults
    // set.
    assertDefaults(checkConns, {readConcern: null, writeConcern: null});

    // Running setDefaultRWConcern should be allowed and should work as expected, i.e. the new
    // defaults should be in the cache and there should be a defaults document.
    assert.commandWorked(conn.adminCommand({
        setDefaultRWConcern: 1,
        defaultReadConcern: {level: "local"},
        defaultWriteConcern: {w: 2, wtimeout: 0}
    }));
    assertDefaults(checkConns, {readConcern: {level: "local"}, writeConcern: {w: 2, wtimeout: 0}});
    assert(conn.getDB("config").settings.findOne({_id: "ReadWriteConcernDefaults"}));
}

jsTestLog("Testing plain replica set...");
{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    runTest(rst.getPrimary(), rst.nodes, rst.getPrimary());

    // Run the test a second time to verify repeated upgrades/downgrades don't lead to problems.
    runTest(rst.getPrimary(), rst.nodes, rst.getPrimary());

    rst.stopSet();
}

jsTestLog("Testing sharded cluster...");
{
    const st = new ShardingTest({mongos: 2, shards: 1});

    runTest(st.s, [st.s0, st.s1, ...st.configRS.nodes], st.configRS.getPrimary());

    // Run the test a second time to verify repeated upgrades/downgrades don't lead to problems.
    runTest(st.s, [st.s0, st.s1, ...st.configRS.nodes], st.configRS.getPrimary());

    st.stop();
}
}());
