/**
 * Tests migrating from getLastErrorDefaults to CWWC.
 * TODO SERVER-56576: Remove this test once we branch 5.0
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

function testReplSet(writeConcern) {
    const replTest = new ReplSetTest({
        nodes: {
            0: {binVersion: "last-lts"},
            1: {binVersion: "last-lts"},
        },
        settings: {getLastErrorDefaults: writeConcern}
    });
    replTest.startSet();
    replTest.initiate();
    let primary = replTest.getPrimary();

    // Test that the current config has the expected fields.
    let config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(config.settings.getLastErrorDefaults.w, writeConcern.w, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.wTimeout, writeConcern.wTimeout, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.j, writeConcern.j, tojson(config));

    jsTestLog("Migrating from GLED to CWWC");
    assert.commandWorked(
        primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: writeConcern}));
    delete config.settings.getLastErrorDefaults;
    config.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    jsTestLog("Upgrading set");
    replTest.upgradeSet({binVersion: "latest"});

    primary = replTest.getPrimary();
    const res = primary.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, writeConcern);
    replTest.stopSet();
}

function testSharding(writeConcern) {
    const st = new ShardingTest({
        shards: {
            rs0: {
                nodes: {
                    0: {binVersion: "last-lts"},
                    1: {binVersion: "last-lts"},
                },
                settings: {getLastErrorDefaults: writeConcern}
            },
            rs1: {
                nodes: {
                    0: {binVersion: "last-lts"},
                    1: {binVersion: "last-lts"},
                }
            }
        },
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    // Test that the current config has the expected fields.
    const primary = st.rs0.getPrimary();
    const config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(config.settings.getLastErrorDefaults.w, writeConcern.w, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.wTimeout, writeConcern.wTimeout, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.j, writeConcern.j, tojson(config));

    jsTestLog("Migrating from GLED to CWWC");
    assert.commandWorked(
        st.s.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: writeConcern}));
    delete config.settings.getLastErrorDefaults;
    config.version++;
    assert.commandWorked(primary.adminCommand({replSetReconfig: config}));

    jsTestLog("Upgrading set");
    st.upgradeCluster('latest');

    const res = st.s.adminCommand({getDefaultRWConcern: 1});
    assert.eq(res.defaultWriteConcern, writeConcern);
    st.stop();
}

testReplSet({w: 2, j: false, wtimeout: 0});
testSharding({w: 1, j: false, wtimeout: 70});
}());
