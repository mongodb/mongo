/**
 * Tests that upgrading/downgrading to/from 5.0 will warn users if no cluster-wide write concern is
 * set.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

function testReplSetUpgrade(setCWWC) {
    clearRawMongoProgramOutput();
    const replTest = new ReplSetTest(
        {nodes: [{binVersion: "last-lts"}, {binVersion: "last-lts"}, {binVersion: "last-lts"}]});
    replTest.startSet();
    replTest.initiate();
    let primary = replTest.getPrimary();
    if (setCWWC) {
        assert.commandWorked(
            primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 'majority'}}));
    }

    jsTestLog("Upgrading replica set to 5.0");
    replTest.upgradeSet({binVersion: "latest"});
    if (setCWWC) {
        assert(rawMongoProgramOutput().search(/5569202.*The default write concern/) == -1,
               'Replica set should not have warning when upgrading to 5.0 if cluster-wide write ' +
                   'concern is set');

    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5569202.*The default write concern'),
            'Replica set should have warning when upgrading to 5.0 if no cluster-wide write ' +
                'concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    primary = replTest.getPrimary();
    jsTestLog("Setting FCV to 5.0");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    if (setCWWC) {
        assert(
            rawMongoProgramOutput().search(/5569200.*The default write concern/) == -1,
            'Replica set should not have warning when upgrading FCV to 5.0 if cluster-wide write' +
                ' concern is set');
    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5569200.*The default write concern'),
            'Replica set should have warning when upgrading FCV to 5.0 if no cluster-wide write' +
                ' concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    replTest.stopSet();
}

function testReplSetDowngrade(setCWWC) {
    clearRawMongoProgramOutput();
    const replTest = new ReplSetTest({nodes: [{}, {}, {}]});
    replTest.startSet();
    replTest.initiate();
    const primary = replTest.getPrimary();
    if (setCWWC) {
        assert.commandWorked(
            primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}}));
    }

    jsTestLog("Downgrading FCV from 5.0");
    assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    if (setCWWC) {
        assert(rawMongoProgramOutput().search(/5569201.*The default write concern/) == -1,
               'Replica set should not have warning when downgrading FCV from 5.0 if cluster-wide' +
                   ' write concern is set');
    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5569201.*The default write concern'),
            'Replica set should have warning when downgrading FCV from 5.0 if no cluster-wide' +
                ' write concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    replTest.upgradeSet({binVersion: "last-lts"});
    replTest.stopSet();
}

function testShardingUpgrade(setCWWC) {
    clearRawMongoProgramOutput();
    const st = new ShardingTest({
        shards: {
            rs0: {
                nodes:
                    [{binVersion: "last-lts"}, {binVersion: "last-lts"}, {binVersion: "last-lts"}]
            },
            rs1: {
                nodes:
                    [{binVersion: "last-lts"}, {binVersion: "last-lts"}, {binVersion: "last-lts"}]
            }
        },
        config: 1,
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    if (setCWWC) {
        assert.commandWorked(
            st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}}));
    }

    jsTestLog("Upgrading sharded cluster to 5.0");
    st.upgradeCluster('latest');
    if (setCWWC) {
        assert(
            rawMongoProgramOutput().search(/5569202.*The default write concern/) == -1,
            'Sharded cluster should not have warning when upgrading to 5.0 if cluster-wide write' +
                ' concern is set');

    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5569202.*The default write concern'),
            'Sharded cluster should have warning when upgrading to 5.0 if no cluster-wide write' +
                ' concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    jsTestLog("Setting FCV to 5.0");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    if (setCWWC) {
        assert(
            rawMongoProgramOutput().search(/5569200.*The default write concern/) == -1,
            'Sharded cluster should not have warning when upgrading FCV to 5.0 if cluster-wide ' +
                'write concern is set');
    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5569200.*The default write concern'),
            'Sharded cluster should have warning when upgrading FCV to 5.0 if no cluster-wide ' +
                'write concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    st.stop();
}

function testShardingDowngrade(setCWWC) {
    clearRawMongoProgramOutput();
    const st = new ShardingTest({
        shards: {rs0: {nodes: [{}, {}, {}]}, rs1: {nodes: [{}, {}, {}]}},
        config: 1,
    });

    if (setCWWC) {
        assert.commandWorked(
            st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 'majority'}}));
    }

    jsTestLog("Downgrading FCV from 5.0");

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    if (setCWWC) {
        assert(rawMongoProgramOutput().search(/5569201.*The default write concern/) == -1,
               'Sharded cluster should not have warning when downgrading FCV from 5.0 if ' +
                   'cluster-wide write concern is set');
    } else {
        assert.soon(() => rawMongoProgramOutput().match('5569201.*The default write concern'),
                    'Sharded cluster should have warning when downgrading FCV from 5.0 if no ' +
                        'cluster-wide write concern is set',
                    ReplSetTest.kDefaultTimeoutMS);
    }

    st.upgradeCluster('last-lts');
    st.stop();
}

for (const setCWWC of [true, false]) {
    testReplSetUpgrade(setCWWC);
    testReplSetDowngrade(setCWWC);
    testShardingUpgrade(setCWWC);
    testShardingDowngrade(setCWWC);
}
}());
