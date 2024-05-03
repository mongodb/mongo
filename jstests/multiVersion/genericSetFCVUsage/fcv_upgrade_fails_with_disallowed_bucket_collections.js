/**
 * Test that FCV upgrade fails in the presence of bucket collections wihtout timeseries options
 */

const latest = "latest";
const testName = "fcv_upgrade_fails_with_disallowed_bucket_collections";
const dbpath = MongoRunner.dataPath + testName;
const dbName = `db_${testName}`;
const bucketCollName = "system.buckets.coll";
const collName = "coll";

function testUpgradeFromFCV(conn, fromFCV) {
    const db = conn.getDB(dbName);

    // Downgrade to fromFCV version
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: fromFCV, confirm: true}));

    // Create a normal collection
    db.createCollection('normalColl');

    // Create bucket collection wihtout timeseries options
    db.createCollection(bucketCollName);

    assert.commandWorked(db[bucketCollName].insertOne({doc: 1}));

    // Upgrade should fail because we have an invalid bucket collection
    assert.commandFailedWithCode(
        db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}),
        ErrorCodes.CannotUpgrade);

    // Rename invalid bucket collection
    assert.commandWorked(db.adminCommand(
        {renameCollection: `${dbName}.${bucketCollName}`, to: `${dbName}.${collName}`}));

    // Upgrade should work since we renamed the offending collection
    assert.commandWorked(
        db.adminCommand({setFeatureCompatibilityVersion: latestFCV, confirm: true}));

    assert.eq(1, db[collName].countDocuments({}));
}

function testAllTopologies(fromFCV) {
    {
        jsTest.log("Testing upgrade with standalone");
        let conn = MongoRunner.runMongod({dbpath: dbpath});

        testUpgradeFromFCV(conn, fromFCV);

        MongoRunner.stopMongod(conn);
    }

    {
        jsTest.log("Testing upgrade with replicaset");
        const rst = new ReplSetTest({nodes: 2});
        rst.startSet();
        rst.initiate();

        testUpgradeFromFCV(rst.getPrimary(), fromFCV);
        rst.stopSet();
    }

    {
        jsTest.log("Testing upgrade with sharded cluster");
        const st = new ShardingTest({shards: 2});

        testUpgradeFromFCV(st.s, fromFCV);

        st.stop();
    }
}

testAllTopologies(lastContinuousFCV);
testAllTopologies(lastLTSFCV);
