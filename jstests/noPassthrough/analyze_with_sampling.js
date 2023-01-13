(function() {
"use strict";

const conn = MongoRunner.runMongod({setParameter: {featureFlagCommonQueryFramework: true}});
assert.neq(null, conn, "mongod was unable to start up");

const db = conn.getDB(jsTestName());

const coll = db.cqf_test_analyze_sample;
const statscoll = db.system.statistics.cqf_test_analyze_sample;
coll.drop();
statscoll.drop();

let data = [];
for (let i = 0; i < 10000; i++) {
    data.push({_id: i, x: i});
}

assert.commandWorked(coll.insertMany(data));

assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x"}));
const fullStats = statscoll.find({_id: "x"}).toArray()[0];
const fullBounds = fullStats.statistics.scalarHistogram.bounds;

assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x", sampleRate: 0.01}));
const sampleStats = statscoll.find({_id: "x"}).toArray()[0];
const sampleBounds = sampleStats.statistics.scalarHistogram.bounds;

// Use histogram bounds as a proxy to verify that sampling occured
assert.neq(fullBounds, sampleBounds);
assert.eq(0.01, sampleStats.statistics.sampleRate);
// Verify that roughly 100 documents have been sampled. Because this is not deterministic, we give
// a sufficiently large range to avoid flakiness in the test, but still give us confidence that we
// are not analyzing the entire collection.
assert.betweenIn(50, sampleStats.statistics.documents, 150);

// Test sampleSize
assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x", sampleSize: 1000}));
const sampleSizeStats = statscoll.find({_id: "x"}).toArray()[0];
assert.eq(0.1, sampleSizeStats.statistics.sampleRate);

assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x", sampleSize: 100000000}));
const sampleSizeFullStats = statscoll.find({_id: "x"}).toArray()[0];
assert.eq(1.0, sampleSizeFullStats.statistics.sampleRate);

// Test sampling on empty collection
assert.commandWorked(coll.deleteMany({}));
assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x", sampleRate: 0.5}));
assert.commandWorked(db.runCommand({analyze: coll.getName(), key: "x", sampleSize: 1000}));

MongoRunner.stopMongod(conn);
}());
