/**
 * Tests for serverStatus profiler stats.
 */
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(jsTest.name());
const coll = db[jsTest.name()];

const nDocs = 32;
coll.drop();
assert.commandWorked(db.createCollection(jsTest.name()));

for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Enable profiling and run a query that implicitly creates the profile collection.
assert.commandWorked(db.setProfilingLevel(2, 0));
assert.eq(nDocs, coll.find({}).itcount());

// Turn off the profiler to avoid incrementing the stats further.
assert.commandWorked(db.setProfilingLevel(0, 0));

let profilerEntries = db.system.profile.find({}).itcount();
assert.eq(1, profilerEntries);

let profilerWriteStats = db.serverStatus().profiler;
assert.eq(0, profilerWriteStats.activeWriters);
assert.eq(profilerWriteStats.totalWrites, profilerEntries);

MongoRunner.stopMongod(conn);
