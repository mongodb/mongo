/**
 * Test to ensure that：
 *      1. The FCV cannot be downgraded to 6.0 if there are queryable range encryption indexes.
 *      2. The FCV can be set back to upgraded if feature flag DowngradingToUpgrading is true.
 *
 * @tags: [requires_fcv_70]
 */
(function() {
"use strict";

load('jstests/libs/collection_drop_recreate.js');
load("jstests/libs/feature_flag_util.js");

const latest = "latest";
const dbName = "test_set_fcv_encrypted_field";
const collName = "encrypted";
const encryptedFieldsOption = {
    encryptedFields: {
        fields: [{
            path: "firstName",
            keyId: UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            bsonType: "int",
            queries: {queryType: "rangePreview", sparsity: 1, min: NumberInt(1), max: NumberInt(2)}
        }]
    }
};

function checkFCVDowngradeUpgrade(db, adminDB) {
    let runDowngradingToUpgrading = false;
    if (FeatureFlagUtil.isEnabled(adminDB, "DowngradingToUpgrading")) {
        runDowngradingToUpgrading = true;
    }

    jsTest.log("Create a encrypted field collection and attempt to setFCV to lastLTS");
    checkFCV(adminDB, latestFCV);
    assertCreateCollection(db, collName, encryptedFieldsOption);
    assert.commandFailedWithCode(adminDB.runCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);

    // Check FCV is in downgrading state.
    checkFCV(adminDB, lastLTSFCV, lastLTSFCV);

    if (runDowngradingToUpgrading) {
        jsTest.log("Set FCV back to latest");
        assert.commandWorked(adminDB.runCommand({setFeatureCompatibilityVersion: latestFCV}));
        checkFCV(adminDB, latestFCV);

        // Check encryptedField is unaffected.
        const res = db.getCollectionInfos({name: collName});
        assert.eq(res[0].options.encryptedFields.fields[0].queries.queryType,
                  encryptedFieldsOption.encryptedFields.fields[0].queries.queryType);
    }

    assertDropCollection(db, collName);
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

runReplicaSetTest();
runShardingTest();
})();
