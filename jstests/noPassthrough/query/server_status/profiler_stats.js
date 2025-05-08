/**
 * Tests for serverStatus metrics.queryExecutor stats.
 */
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB(jsTest.name());
const coll = db[jsTest.name()];

let getProfilerCollectionScansStats = () => {
    return db.serverStatus().metrics.queryExecutor.profiler.collectionScans;
};

let getProfilerWriteStats = () => {
    return db.serverStatus().profiler;
};

// Create and populate a capped collection so that we can run tailable queries.
const nDocs = 32;
coll.drop();
assert.commandWorked(db.createCollection(jsTest.name()));

for (let i = 0; i < nDocs; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Enable profiling and run a query that implicitly creates the profile collection.
assert.commandWorked(db.setProfilingLevel(2, 0));
assert.eq(nDocs, coll.find({}).itcount());

// Test a non-tailable collection scan.
assert.eq(1, db.system.profile.find({}).itcount());
let profilerStats = getProfilerCollectionScansStats();
profilerStats = getProfilerCollectionScansStats();
assert.eq(1, profilerStats.total);
assert.eq(1, profilerStats.nonTailable);
assert.eq(0, profilerStats.tailable);

// Test a tailable collection scan.
assert.commandWorked(db.runCommand(
    {find: "system.profile", filter: {}, tailable: true, awaitData: true, batchSize: 0}));
profilerStats = getProfilerCollectionScansStats();
assert.eq(2, profilerStats.total);
assert.eq(1, profilerStats.nonTailable);
assert.eq(1, profilerStats.tailable);

let profilerWriteStats = getProfilerWriteStats();
assert.eq(0, profilerWriteStats.activeWriters);
assert.eq(profilerWriteStats.totalWrites, db.system.profile.find({}).itcount());

MongoRunner.stopMongod(conn);
