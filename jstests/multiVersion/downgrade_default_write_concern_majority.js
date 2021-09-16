/**
 * Tests downgrading from 5.0 with and without the cluster-wide write concern set.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

function testReplSet(CWWCSet, isPSASet) {
    jsTestLog("Running replica set test with CWWCSet: " + tojson(CWWCSet) +
              ", isPSASet: " + tojson(isPSASet));
    let replSetNodes = 2;
    if (isPSASet) {
        replSetNodes = [{}, {}, {arbiter: true}];
    }
    const replTest = new ReplSetTest({
        nodes: replSetNodes,
    });
    replTest.startSet();
    replTest.initiate();
    let primary = replTest.getPrimary();

    if (CWWCSet) {
        assert.commandWorked(primary.adminCommand(
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority", wtimeout: 0}}));
    }

    let res = primary.adminCommand({getDefaultRWConcern: 1});
    if (!CWWCSet && isPSASet) {
        assert(!res.defaultWriteConcern);
    } else {
        assert.eq(res.defaultWriteConcern, {w: "majority", wtimeout: 0});
    }

    const defaultWriteConcernSource = CWWCSet ? "global" : "implicit";
    assert.eq(res.defaultWriteConcernSource, defaultWriteConcernSource);

    jsTestLog("Downgrading set");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
    replTest.upgradeSet({binVersion: "4.4"});

    primary = replTest.getPrimary();
    res = primary.adminCommand({getDefaultRWConcern: 1});
    if (CWWCSet) {
        assert.eq(res.defaultWriteConcern, {w: "majority", wtimeout: 0});
    } else {
        assert(!res.defaultWriteConcern);
    }
    assert(!res.defaultWriteConcernSource);

    replTest.stopSet();
}

function testSharding(CWWCSet, isPSASet) {
    jsTestLog("Running sharding test with CWWCSet: " + tojson(CWWCSet) +
              ", isPSASet: " + tojson(isPSASet));
    let replSetNodes = 2;
    if (isPSASet) {
        replSetNodes = [{}, {}, {arbiter: true}];
    }
    const st = new ShardingTest({
        shards: {rs0: {nodes: replSetNodes}, rs1: {nodes: replSetNodes}},
        mongos: {nodes: replSetNodes}
    });

    if (CWWCSet) {
        assert.commandWorked(st.s.adminCommand(
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: "majority", wtimeout: 0}}));
    }
    let res = st.s.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, {w: "majority", wtimeout: 0});
    const defaultWriteConcernSource = CWWCSet ? "global" : "implicit";
    assert.eq(res.defaultWriteConcernSource, defaultWriteConcernSource);

    jsTestLog("Downgrading set");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: "4.4"}));
    st.upgradeCluster('4.4');

    res = st.s.adminCommand({getDefaultRWConcern: 1});
    if (CWWCSet) {
        assert.eq(res.defaultWriteConcern, {w: "majority", wtimeout: 0});
    } else {
        assert(!res.defaultWriteConcern);
    }
    assert(!res.defaultWriteConcernSource);
    st.stop();
}

for (const CWWCSet of [true, false]) {
    for (const isPSASet of [true, false]) {
        testReplSet(CWWCSet, isPSASet);
        testSharding(CWWCSet, isPSASet);
    }
}
}());
