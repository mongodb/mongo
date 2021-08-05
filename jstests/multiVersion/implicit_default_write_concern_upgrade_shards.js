/**
 * Tests upgrading a shardsvr to 5.0 binary version will fail if the implicitDefaultWriteConcern is
 * w:1 and CWWC is not set.
 *
 */

(function() {
"use strict";

load("jstests/multiVersion/libs/multi_cluster.js");  // For 'upgradeCluster()'

// This test triggers an unclean shutdown, which may cause inaccurate fast counts.
TestData.skipEnforceFastCountOnValidate = true;

// Checks involve connecting to a shard node, that under certain circumstances would fassert.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

function testSharding(CWWCSet, isPSASet) {
    jsTestLog("Running sharding test with CWWCSet: " + tojson(CWWCSet) +
              ", isPSASet: " + tojson(isPSASet));
    let replSetNodes = [{binVersion: "last-lts"}, {binVersion: "last-lts"}];
    if (isPSASet) {
        replSetNodes = [
            {binVersion: "last-lts"},
            {binVersion: "last-lts"},
            {binVersion: "last-lts", arbiter: true}
        ];
    }
    const st = new ShardingTest({
        shards: {rs0: {nodes: replSetNodes}},
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    if (CWWCSet) {
        jsTestLog("Setting the CWWC before upgrading sharded cluster");
        assert.commandWorked(st.s.adminCommand(
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority", wtimeout: 0}}));
    }

    let cwwc = st.s.adminCommand({getDefaultRWConcern: 1});
    if (CWWCSet) {
        assert.eq(cwwc.defaultWriteConcern, {w: "majority", wtimeout: 0});
    } else {
        assert(!cwwc.defaultWriteConcern);
    }
    assert(!cwwc.defaultWriteConcernSource);

    jsTestLog("Attempting to upgrade sharded cluster.");
    if (!CWWCSet && isPSASet) {
        // Fassert should be raised.
        assert(function() {
            try {
                st.upgradeCluster("latest");
                return false;
            } catch (ex) {
                assert.soon(
                    () => rawMongoProgramOutput().search(/Fatal assertion.*5684400/) >= 0,
                    "Node should have fasserted when upgrading to 5.0 when implicitDefaultWC = 1 and no" +
                        "CWWC is set.",
                    ReplSetTest.kDefaultTimeoutMS);
                return true;
            }
        });

        st.stop({skipValidatingExitCode: true, skipValidation: true});
        return;
    }

    st.upgradeCluster("latest");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    cwwc = st.s.adminCommand({getDefaultRWConcern: 1});
    assert.eq(cwwc.defaultWriteConcern, {w: "majority", wtimeout: 0}, tojson(cwwc));
    const defaultWriteConcernSource = CWWCSet ? "global" : "implicit";
    assert.eq(cwwc.defaultWriteConcernSource, defaultWriteConcernSource);
    st.stop();
}

for (const CWWCSet of [true, false]) {
    for (const isPSASet of [false, true]) {
        testSharding(CWWCSet, isPSASet);
    }
}
})();
