/**
 * Test which verifies that large profiler entries generated for SBE plans do not exceed the max
 * BSON depth. Instead, they get truncated right below the max depth.
 */
(function() {
"use strict";

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to startup");
const db = conn.getDB("test");
const collName = jsTestName();
const coll = db[collName];
coll.drop();

// Insert some documents so our query will perform some work.
assert.commandWorked(coll.insert([{a: 1}, {a: 2}]));
const longField = 'a.'.repeat(99) + 'a';
const projectionSpec = {
    [longField]: 1
};

// Setup the profiler to only pick up the query below.
assert.commandWorked(db.setProfilingLevel(2, {slowms: 0, sampleRate: 1}, {
    filter: {'op': 'query', 'command.projection': projectionSpec}
}));

// Verify that our query was picked up by the profiler.
coll.find({}, projectionSpec).toArray();
const profilerEntry = db.system.profile.find().toArray();
assert.eq(1, profilerEntry.length, profilerEntry);

// Collection validation should detect no issues.
assert.commandWorked(db.system.profile.validate({full: true}));
MongoRunner.stopMongod(conn);
}());
