/**
 * Tests upgrading to/downgrading from 5.0 with and without the cluster-wide read concern set.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

function testShardingUpgrade(CWRCSet) {
    jsTestLog("Running sharding test with CWRCSet: " + tojson(CWRCSet));
    const replSetNodes = [{binVersion: "last-lts"}, {binVersion: "last-lts"}];
    const st = new ShardingTest({
        shards: {rs0: {nodes: replSetNodes}, rs1: {nodes: replSetNodes}},
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    if (CWRCSet) {
        jsTestLog("Setting the CWRC before upgrade for sharded cluster");
        assert.commandWorked(
            st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "available"}}));
    }

    let res = st.s.adminCommand({getDefaultRWConcern: 1});
    if (CWRCSet) {
        assert.eq(res.defaultReadConcern, {level: "available"});
    } else {
        assert(!res.defaultReadConcern);
    }
    assert(!res.defaultReadConcernSource);

    jsTestLog("Attempting to upgrade sharded cluster");
    st.upgradeCluster("latest");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));

    res = st.s.adminCommand({getDefaultRWConcern: 1});
    const defaultReadConcern = CWRCSet ? {level: "available"} : {level: "local"};
    assert.eq(res.defaultReadConcern, defaultReadConcern, tojson(res));
    const defaultReadConcernSource = CWRCSet ? "global" : "implicit";
    assert.eq(res.defaultReadConcernSource, defaultReadConcernSource);

    st.stop();
}

function testShardingDowngrade(CWRCSet) {
    jsTestLog("Running sharding test with CWRCSet: " + tojson(CWRCSet));
    const st = new ShardingTest({shards: {rs0: {nodes: 2}, rs1: {nodes: 2}}});

    if (CWRCSet) {
        assert.commandWorked(
            st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "majority"}}));
    }
    let res = st.s.adminCommand({getDefaultRWConcern: 1});
    const defaultReadConcern = CWRCSet ? {level: "majority"} : {level: "local"};
    assert.eq(res.defaultReadConcern, defaultReadConcern);

    const defaultReadConcernSource = CWRCSet ? "global" : "implicit";
    assert.eq(res.defaultReadConcernSource, defaultReadConcernSource);

    jsTestLog("Downgrading sharded cluster");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    st.upgradeCluster('last-lts');

    res = st.s.adminCommand({getDefaultRWConcern: 1});
    if (CWRCSet) {
        assert.eq(res.defaultReadConcern, {level: "majority"});
    } else {
        assert(!res.defaultReadConcern);
    }
    assert(!res.defaultReadConcernSource);
    st.stop();
}

for (const CWRCSet of [true, false]) {
    testShardingUpgrade(CWRCSet);
    testShardingDowngrade(CWRCSet);
}
}());
