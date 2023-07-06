/**
 * Tests that when the profiler (introspect.cpp) wants to log an operation but the
 * 'system.profiler' collection does not yet exist and the operation context is an interrupted
 * state, the collection can still be successfully created on the fly.
 *
 * This test restarts the server and requires that data persists across restarts.
 * @tags: [requires_persistence, requires_profiling]
 */
//
// Start mongo with profiling disabled, create an empty database, and populate it with a
// collection that has one document.
//
let standalone = MongoRunner.runMongod({profile: "0"});

let db = standalone.getDB("profile_interrupted_op");
assert.commandWorked(db.dropDatabase());

let coll = db.getCollection("test");
assert.commandWorked(coll.insert({a: 1}));

//
// Stop the mongod and then restart it, this time with profiling enabled. Note that enabling
// profiling on a running database would create the 'system.profile' collection, which we don't
// yet want created for this test.
//
MongoRunner.stopMongod(standalone);
standalone = MongoRunner.runMongod(
    {restart: true, cleanData: false, dbpath: standalone.dbpath, profile: "2"});

//
// Execute a query that will get interrupted for exceeding its 'maxTimeMS' value. The profiler
// will attempt to create the 'system.profile' collection while the operation context is already
// marked as interrupted.
//
db = standalone.getDB("profile_interrupted_op");
coll = db.getCollection("test");
const err = assert.throws(function() {
    coll.find({
            $where: function() {
                sleep(3600);
                return true;
            }
        })
        .maxTimeMS(1000)
        .count();
});
assert.contains(
    err.code, [ErrorCodes.MaxTimeMSExpired, ErrorCodes.Interrupted, ErrorCodes.InternalError], err);

// The mongod should have created the 'system.profile' collection automatically.
const res = db.runCommand({listCollections: 1, filter: {name: "system.profile"}});
assert.commandWorked(res);
assert.eq(res.cursor.firstBatch.length, 1, res);

MongoRunner.stopMongod(standalone);