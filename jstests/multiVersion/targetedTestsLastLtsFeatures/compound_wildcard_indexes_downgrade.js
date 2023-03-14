/**
 * Tests that we will fail on startup if the compound wildcard indexes were not removed before we
 * downgrade from 7.0 to 'last-lts'. Downgrading FCV will allow continued use of a CWI as long as
 * the version of mongod is still 7.0, but will disallow any new creation of a CWI.
 *
 * @tags: [
 *   featureFlagCompoundWildcardIndexes,
 *   requires_fcv_70,
 * ]
 */

(function() {
'use strict';

load("jstests/libs/analyze_plan.js");  // For getPlanStages.

const dbpath = MongoRunner.dataPath + 'compound_wildcard_indexes_downgrade';
resetDbpath(dbpath);

// If we have a CWI on the admin database, we want to make sure we can startup properly despite FCV
// not being initialized yet. It's possible to hit an invariant if featureFlag.isEnabled is called
// without checking fcv.isVersionInitialized.
const dbName = 'admin';
const dbNameTest = "compound_wildcard_indexes_downgrade";
const collName = 'compound_wildcard_indexes_downgrade';

const latestVersion = "latest";
const lastLTSVersion = "last-lts";

const keyPattern = {
    "a.$**": 1,
    b: 1
};

// Startup with latest, create a compound wildcard index, stop mongod.
{
    const conn =
        MongoRunner.runMongod({dbpath: dbpath, binVersion: latestVersion, noCleanData: true});
    const db = conn.getDB(dbName);
    const coll = db[collName];

    assert.commandWorked(coll.createIndex(keyPattern));

    assert.commandWorked(coll.insert({a: {c: 1}, b: 1}));
    assert.commandWorked(coll.insert({a: 30, b: 20}));

    MongoRunner.stopMongod(conn);
}

// Test that we are able to restart a mongod if there exists any CWI on the 'admin' DB and the FCV
// may not be initialized.
{
    const conn =
        MongoRunner.runMongod({dbpath: dbpath, binVersion: latestVersion, noCleanData: true});
    const db = conn.getDB(dbName);
    const coll = db[collName];

    // Drop the CWI for downgrading.
    assert.commandWorked(coll.dropIndex(keyPattern));

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: '6.0'}));
    MongoRunner.stopMongod(conn);
}

// A normal downgrade process should drop all CWI. Now there's no CWI, we should be able to start a
// last-lts mongod.
{
    const conn =
        MongoRunner.runMongod({dbpath: dbpath, binVersion: lastLTSVersion, noCleanData: true});

    MongoRunner.stopMongod(conn);
}

// Tests on a regular database. Test that 1) FCV can be downgraded with the existence of CWI, 2)
// continued use of CWI after FCV downgraded, 3) cannot create more CWI, and 4) a downgraded mongod
// fails to start up if CWI is not removed.
{
    let conn =
        MongoRunner.runMongod({dbpath: dbpath, binVersion: latestVersion, noCleanData: true});
    let db = conn.getDB(dbNameTest);
    let coll = db[collName];
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: '7.0'}));

    assert.commandWorked(coll.createIndex(keyPattern));

    // Test that it succeeds to downgrade the FCV with the existence of CWI, but it should fail to
    // start a mongod with the existence of a CWI.
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: '6.0'}));

    // Test that the CWI can still be used after FCV downgraded.
    const exp = coll.find({"a.c": 1}).explain();
    const winningPlan = getWinningPlan(exp.queryPlanner);
    const ixScans = getPlanStages(winningPlan, "IXSCAN");
    assert.gt(ixScans.length, 0, exp);
    assert.docEq(ixScans[0].indexName, "a.$**_1_b_1", ixScans);

    // We cannot create more CWI if FCV is below 7.0.
    assert.commandFailedWithCode(coll.createIndex({"b.$**": 1, c: 1}),
                                 ErrorCodes.CannotCreateIndex);

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: '7.0'}));

    // We can create more CWI if FCV is 7.0.
    assert.commandWorked(coll.createIndex({"b.$**": 1, c: 1}));

    MongoRunner.stopMongod(conn);

    // To successfully downgrade a mongod, user must drop all CWI first.
    assert.throws(() => MongoRunner.runMongod(
                      {dbpath: dbpath, binVersion: lastLTSVersion, noCleanData: true}),
                  [],
                  "MongoD should fail because wildcard indexes do not allow compounding");

    // Start a "latest" mongod and drop all indexes to successfully downgrade the mongod.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: latestVersion, noCleanData: true});
    db = conn.getDB(dbNameTest);
    coll = db[collName];
    coll.dropIndexes();

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: '6.0'}));

    MongoRunner.stopMongod(conn);

    // We can downgrade now as all indexes have been removed.
    conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastLTSVersion, noCleanData: true});

    MongoRunner.stopMongod(conn);
}
})();
