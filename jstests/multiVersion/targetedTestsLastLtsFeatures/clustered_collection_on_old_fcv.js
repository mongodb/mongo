/**
 * Test that mongod will not allow creation of new clustered collections but still allow
 * interactions with existing ones if downgraded to an old version.
 *
 * @tags: []
 */

(function() {
"use strict";

const kClusteredCollName = 'clusteredColl';
const dbpath = MongoRunner.dataPath + 'clustered_collection';

let conn = MongoRunner.runMongod({dbpath: dbpath, binVersion: "latest"});
let db = conn.getDB(jsTestName());

db.adminCommand({setFeatureCompatibilityVersion: latestFCV});

assert.commandWorked(
    db.createCollection(kClusteredCollName, {clusteredIndex: {key: {_id: 1}, unique: true}}));

assert.commandWorked(db[kClusteredCollName].insertOne({_id: 'latest', info: "my latest doc"}));

db.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV});

assert.commandWorked(db[kClusteredCollName].insertOne({_id: 'new', info: "my new doc"}));

let doc = db[kClusteredCollName].findOne({_id: 'latest'});
assert.eq(doc.info, "my latest doc");

assert.commandFailed(db.createCollection(kClusteredCollName + "_failed",
                                         {clusteredIndex: {key: {_id: 1}, unique: true}}));

db.adminCommand({setFeatureCompatibilityVersion: latestFCV});

MongoRunner.stopMongod(conn);
}());
