/**
 * Test to ensure that：
 *      1. The FCV cannot be downgraded to 6.0 if there are capped collections with a size
 *         that is non multiple of 256 bytes.
 *      2. The FCV can be set back to upgraded if feature flag DowngradingToUpgrading is true.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');
load("jstests/libs/feature_flag_util.js");

const latest = "latest";
const dbName = "test_set_fcv_capped_collection";
const collName = "capped_collection";
const cappedCollOptions = {
    capped: true,
    size: 5242881,
    max: 5000,
};

function checkFCVDowngradeUpgrade(db, adminDB) {
    let runDowngradingToUpgrading = false;
    if (FeatureFlagUtil.isEnabled(adminDB, "DowngradingToUpgrading")) {
        runDowngradingToUpgrading = true;
    }

    jsTest.log("Create a relaxed size capped collection and attempt to setFCV to lastLTS");
    checkFCV(adminDB, latestFCV);
    assertCreateCollection(db, collName, cappedCollOptions);
    assert.commandFailedWithCode(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);

    // Check FCV is in downgrading state.
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV);

    if (runDowngradingToUpgrading) {
        jsTest.log("Set FCV back to latest");
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);

        // Confirm the capped collection is not affected.
        const res = db[collName].stats();
        assert.eq(res.capped, cappedCollOptions.capped);
        assert.eq(res.maxSize, cappedCollOptions.size);
    }

    assertDropCollection(db, collName);
}

function runStandaloneTest() {
    jsTest.log("Start Standalone test");
    const conn = MongoRunner.runMongod({binVersion: latest});
    const db = conn.getDB(dbName);
    const adminDB = conn.getDB("admin");

    checkFCVDowngradeUpgrade(db, adminDB);

    MongoRunner.stopMongod(conn);
}

function runReplicaSetTest() {
    jsTest.log("Start Replica Set test");
    const rst = new ReplSetTest({nodes: 2, nodeOpts: {binVersion: latest}});
    rst.startSet();
    rst.initiate();
    const db = rst.getPrimary().getDB(dbName);
    const adminDB = rst.getPrimary().getDB("admin");

    checkFCVDowngradeUpgrade(db, adminDB);

    rst.stopSet();
}

function runShardingTest() {
    jsTest.log("Start Sharding test");
    const st = new ShardingTest({shards: 2, mongos: 1, config: 1});
    const db = st.s.getDB(dbName);
    const adminDB = st.s.getDB("admin");

    checkFCVDowngradeUpgrade(db, adminDB);

    st.stop();
}

runStandaloneTest();
runReplicaSetTest();
runShardingTest();
})();
