/**
 * Tests upgrading from 4.4 to 5.0 with and without the cluster-wide write concern set.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

function testReplSet(CWWCSet, isPSASet) {
    jsTestLog("Running replica set test with CWWCSet: " + tojson(CWWCSet) +
              ", isPSASet: " + tojson(isPSASet));
    let replSetNodes = [{binVersion: "last-lts"}, {binVersion: "last-lts"}];
    if (isPSASet) {
        replSetNodes = [
            {binVersion: "last-lts"},
            {binVersion: "last-lts"},
            {binVersion: "last-lts", arbiter: true}
        ];
    }
    let replTest = new ReplSetTest({
        nodes: replSetNodes,
    });
    replTest.startSet();
    replTest.initiate();
    let primary = replTest.getPrimary();

    if (CWWCSet) {
        jsTestLog("Setting the CWWC before upgrade for replica set");
        assert.commandWorked(primary.adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: "majority"},
            writeConcern: {w: "majority"}
        }));
    }

    let cwwc = primary.adminCommand({getDefaultRWConcern: 1});
    if (CWWCSet) {
        assert.eq(cwwc.defaultWriteConcern, {w: "majority", wtimeout: 0});
    } else {
        assert(!cwwc.defaultWriteConcern);
    }
    assert(!cwwc.defaultWriteConcernSource);

    jsTestLog("Attempting to upgrade replica set");
    replTest.upgradeSet({binVersion: "latest"});
    primary = replTest.getPrimary();
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    cwwc = primary.adminCommand({getDefaultRWConcern: 1});
    if (!CWWCSet && isPSASet) {
        assert(!cwwc.defaultWriteConcern);
    } else {
        assert(cwwc.hasOwnProperty("defaultWriteConcern"));
        assert.eq(cwwc.defaultWriteConcern, {w: "majority", wtimeout: 0}, tojson(cwwc));
    }
    const defaultWriteConcernSource = CWWCSet ? "global" : "implicit";
    assert.eq(cwwc.defaultWriteConcernSource, defaultWriteConcernSource);

    replTest.stopSet();
}

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
        shards: {rs0: {nodes: replSetNodes}, rs1: {nodes: replSetNodes}},
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    if (CWWCSet) {
        jsTestLog("Setting the CWWC before upgrade for sharded cluster");
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

    jsTestLog("Attempting to upgrade sharded cluster");
    st.upgradeCluster("latest");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    cwwc = st.s.adminCommand({getDefaultRWConcern: 1});
    assert.eq(cwwc.defaultWriteConcern, {w: "majority", wtimeout: 0}, tojson(cwwc));
    const defaultWriteConcernSource = CWWCSet ? "global" : "implicit";
    assert.eq(cwwc.defaultWriteConcernSource, defaultWriteConcernSource);

    st.stop();
}

for (const CWWCSet of [true, false]) {
    for (const isPSASet of [true, false]) {
        testReplSet(CWWCSet, isPSASet);
        testSharding(CWWCSet, isPSASet);
    }
}
}());
