/**
 * Tests that we cannot downgrade FCV when we have partial indexes with $or/$in/$geoWithin.
 */

(function() {
'use strict';

const dbpath = MongoRunner.dataPath + 'partial_indexes_downgrade';
resetDbpath(dbpath);

// Start with 6.0, create a partial index with an $or, then make sure we fail to downgrade FCV
// to 5.0. Drop the index, then actually downgrade to 5.0.
{
    const conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: 'latest', noCleanData: true});

    const db = conn.getDB('test');
    const coll = db['partial_indexes_downgrade'];
    assert.commandWorked(coll.createIndex(
        {a: 1, b: 1}, {partialFilterExpression: {$or: [{a: {$lt: 20}}, {b: {$lt: 10}}]}}));

    coll.insert({a: 1, b: 1});
    coll.insert({a: 30, b: 20});

    assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}),
                                 ErrorCodes.CannotDowngrade);
    coll.dropIndexes();
    assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

    MongoRunner.stopMongod(conn);
}

// Startup with 5.0 binary, with FCV set to 5.0.
{
    const conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: lastLTSFCV, noCleanData: true});

    const db = conn.getDB('test');
    const coll = db['partial_indexes_downgrade'];
    // Make sure we are on the same db path as before.
    assert.eq(coll.aggregate().toArray().length, 2);

    MongoRunner.stopMongod(conn);
}
})();