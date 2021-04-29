/**
 * Tests that upgrading a set that has a non-default getLastErrorDefaults field to 'latest'
 * will trigger an fassert on startup.
 * TODO SERVER-56576: Remove upgrade_with_non_default_get_last_error_defaults_fails_on_startup.js
 * once we branch 5.0
 */
(function() {
"use strict";
load("jstests/multiVersion/libs/multi_rs.js");
load('jstests/multiVersion/libs/multi_cluster.js');

// Checking UUID/index consistency and orphans involves talking to shards, but this test shuts down
// one shard.
TestData.skipCheckingUUIDsConsistentAcrossCluster = true;
TestData.skipCheckingIndexesConsistentAcrossCluster = true;
TestData.skipCheckOrphans = true;

function testReplSet(gleDefaults) {
    const replTest = new ReplSetTest({
        nodes: {
            0: {binVersion: "last-lts"},
            1: {binVersion: "last-lts"},
        },
        settings: gleDefaults
    });
    replTest.startSet();
    replTest.initiate();
    const primary = replTest.getPrimary();

    // Test that the current config has the expected fields.
    let config = primary.adminCommand({replSetGetConfig: 1}).config;

    assert.eq(
        config.settings.getLastErrorDefaults.w, gleDefaults.getLastErrorDefaults.w, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.wTimeout,
              gleDefaults.getLastErrorDefaults.wTimeout,
              tojson(config));
    assert.eq(
        config.settings.getLastErrorDefaults.j, gleDefaults.getLastErrorDefaults.j, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.fsync,
              gleDefaults.getLastErrorDefaults.fsync,
              tojson(config));

    clearRawMongoProgramOutput();
    jsTestLog("Attempting to upgrade set");
    assert.soon(
        function() {
            try {
                replTest.upgradeSet({binVersion: "latest"});
                return false;
            } catch (ex) {
                return true;
            }
        },
        "Node should fail when starting up with a non-default getLastErrorDefaults field",
        ReplSetTest.kDefaultTimeoutMS);

    assert(rawMongoProgramOutput().match('Fatal assertion.*5624100'),
           'Node should fassert when starting up with a non-default getLastErrorDefaults field');

    replTest.stopSet();
}

function testSharding(gleDefaults) {
    const st = new ShardingTest({
        shards: {
            rs0: {
                nodes: {
                    0: {binVersion: "last-lts"},
                    1: {binVersion: "last-lts"},
                },
                settings: gleDefaults
            },
            rs1: {
                nodes: {
                    0: {binVersion: "last-lts"},
                    1: {binVersion: "last-lts"},
                },
                settings: gleDefaults
            }
        },
        other: {mongosOptions: {binVersion: "last-lts"}, configOptions: {binVersion: "last-lts"}}
    });

    // Test that the current config has the expected fields.
    const primary = st.rs0.getPrimary();
    const config = primary.adminCommand({replSetGetConfig: 1}).config;
    assert.eq(
        config.settings.getLastErrorDefaults.w, gleDefaults.getLastErrorDefaults.w, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.wTimeout,
              gleDefaults.getLastErrorDefaults.wTimeout,
              tojson(config));
    assert.eq(
        config.settings.getLastErrorDefaults.j, gleDefaults.getLastErrorDefaults.j, tojson(config));
    assert.eq(config.settings.getLastErrorDefaults.fsync,
              gleDefaults.getLastErrorDefaults.fsync,
              tojson(config));

    clearRawMongoProgramOutput();
    jsTestLog("Attempting to upgrade set");
    assert.soon(
        function() {
            try {
                st.upgradeCluster(
                    'latest', {upgradeShards: true, upgradeConfigs: false, upgradeMongos: false});
                return false;
            } catch (ex) {
                return true;
            }
        },
        "Node should fail when starting up with a non-default getLastErrorDefaults field",
        ReplSetTest.kDefaultTimeoutMS);

    assert(rawMongoProgramOutput().match('Fatal assertion.*5624100'),
           'Node should fassert when starting up with a non-default getLastErrorDefaults field');

    st.stop();
}

function runTest(gleDefaults) {
    testReplSet(gleDefaults);
    testSharding(gleDefaults);
}

jsTestLog("Testing getLastErrorDefaults with {w: 'majority'}");
runTest({getLastErrorDefaults: {w: 'majority', wtimeout: 0}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 1}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 1}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 0, j: true}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 0, j: true}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 0, fsync: true}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 0, fsync: true}});

jsTestLog("Testing getLastErrorDefaults with {w:1, wtimeout: 0, j: false}");
runTest({getLastErrorDefaults: {w: 1, wtimeout: 0, j: false}});
}());
