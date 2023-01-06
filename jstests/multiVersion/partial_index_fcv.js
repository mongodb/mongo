/**
 *
 */

(function() {
// 'use strict';

const dbpath = MongoRunner.dataPath + 'partial_index_fcv';
resetDbpath(dbpath);

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: '6.0', noCleanData: true});

db = conn.getDB('test');
coll = db['partial_index_fcv'];
assert.commandWorked(coll.createIndex(
    {a: 1, b: 1}, {partialFilterExpression: {$or: [{a: {$lt: 20}}, {b: {$lt: 10}}]}}))

coll.insert({a: 1, b: 1})
coll.insert({a: 5, b: 6})
coll.insert({a: 1, b: 20})
coll.insert({a: 30, b: 1})
coll.insert({a: 30, b: 20})

assert.commandFailedWithCode(db.adminCommand({setFeatureCompatibilityVersion: '5.0'}),
                             ErrorCodes.CannotDowngrade);
coll.dropIndexes();
assert.commandWorked(db.adminCommand({setFeatureCompatibilityVersion: '5.0'}));

MongoRunner.stopMongod(conn);

conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: '5.0', noCleanData: true});

db = conn.getDB('test');
coll = db['partial_index_fcv'];
assert.eq(coll.aggregate().toArray().length, 5);

MongoRunner.stopMongod(conn);
})();