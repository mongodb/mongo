/**
 * Tests that we don't fail the FCV check for partial indexes on the admin database during startup.
 */

(function() {
'use strict';

const dbpath = MongoRunner.dataPath + 'partial_indexes_downgrade';
resetDbpath(dbpath);

// If we have a partial index on the admin database, we want to make sure we can startup properly
// despite FCV not being initialized yet. It's possible to hit an invariant if featureFlag.isEnabled
// is called without checking fcv.isVersionInitialized (see SERVER-71068 for more details).
const dbName = 'admin';
const collName = 'partial_indexes_downgrade';

// Startup with latest, create partial index, stop mongod.
{
    const conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: 'latest', noCleanData: true});
    const db = conn.getDB(dbName);
    const coll = db[collName];

    assert.commandWorked(coll.createIndex(
        {a: 1, b: 1}, {partialFilterExpression: {$or: [{a: {$lt: 20}}, {b: {$lt: 10}}]}}));

    assert.commandWorked(coll.insert({a: 1, b: 1}));
    assert.commandWorked(coll.insert({a: 30, b: 20}));

    MongoRunner.stopMongod(conn);
}

// Startup with latest again, to make sure we're not checking FCV for this index at startup.
{
    const conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: 'latest', noCleanData: true});
    const db = conn.getDB(dbName);
    const coll = db[collName];

    // Make sure we are on the same db path as before.
    assert.eq(coll.aggregate().toArray().length, 2);

    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));
    MongoRunner.stopMongod(conn);
}
})();
