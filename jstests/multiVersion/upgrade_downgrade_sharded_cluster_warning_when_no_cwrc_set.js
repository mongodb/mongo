/**
 * Tests that upgrading/downgrading a sharded cluster to/from 5.0 will warn users if no cluster-wide
 * read concern is set.
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

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
            st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "local"}}));
    }

    jsTestLog("Upgrading sharded cluster to 5.0");
    st.upgradeCluster('latest');
    if (setCWWC) {
        assert(
            rawMongoProgramOutput().search(/5686202.*The default read concern/) == -1,
            'Sharded cluster should not have warning when upgrading to 5.0 if cluster-wide read' +
                ' concern is set');

    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5686202.*The default read concern'),
            'Sharded cluster should have warning when upgrading to 5.0 if no cluster-wide read' +
                ' concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    jsTestLog("Setting FCV to 5.0");
    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    if (setCWWC) {
        assert(
            rawMongoProgramOutput().search(/5686200.*The default read concern/) == -1,
            'Sharded cluster should not have warning when upgrading FCV to 5.0 if cluster-wide ' +
                'read concern is set');
    } else {
        assert.soon(
            () => rawMongoProgramOutput().match('5686200.*The default read concern'),
            'Sharded cluster should have warning when upgrading FCV to 5.0 if no cluster-wide ' +
                'read concern is set',
            ReplSetTest.kDefaultTimeoutMS);
    }

    st.stop();
}

function testShardingDowngrade(setCWWC) {
    clearRawMongoProgramOutput();
    const st = new ShardingTest({
        shards: {rs0: {nodes: 2}, rs1: {nodes: 2}},
        config: 1,
    });

    if (setCWWC) {
        assert.commandWorked(
            st.s.adminCommand({setDefaultRWConcern: 1, defaultReadConcern: {level: "majority"}}));
    }

    jsTestLog("Downgrading FCV from 5.0");

    assert.commandWorked(st.s.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    if (setCWWC) {
        assert(rawMongoProgramOutput().search(/5686201.*The default read concern/) == -1,
               'Sharded cluster should not have warning when downgrading FCV from 5.0 if ' +
                   'cluster-wide read concern is set');
    } else {
        assert.soon(() => rawMongoProgramOutput().match('5686201.*The default read concern'),
                    'Sharded cluster should have warning when downgrading FCV from 5.0 if no ' +
                        'cluster-wide read concern is set',
                    ReplSetTest.kDefaultTimeoutMS);
    }

    st.upgradeCluster('last-lts');
    st.stop();
}

for (const setCWWC of [true, false]) {
    testShardingUpgrade(setCWWC);
    testShardingDowngrade(setCWWC);
}
}());
