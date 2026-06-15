/**
 * A profile filter whose $expr contains a $convert (which must perform a feature flag check)
 * must not crash the server during profiling.
 */
const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod failed to start");

const testDB = conn.getDB(jsTestName());

assert.commandWorked(
    testDB.runCommand({
        profile: 2,
        filter: {$expr: {$gte: [{$convert: {input: "$millis", to: "long"}}, NumberLong(0)]}},
    }),
);

// Run operations that get profiled. Before SERVER-128558, evaluating the profile filter's $convert
// dereferences the detached (nullptr) opCtx and crashes the server.
const coll = testDB.getCollection("c");
assert.commandWorked(coll.insert({_id: 1, a: 1}));
assert.eq(1, coll.find({_id: 1}).itcount());

// The server is still up and the filter matched the profiled operations.
assert.gt(testDB.system.profile.find({}).itcount(), 0);

MongoRunner.stopMongod(conn);
