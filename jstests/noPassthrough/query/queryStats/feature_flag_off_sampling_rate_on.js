/**
 * Test that calls to read from query stats store fail when feature flag is turned off and sampling
 * rate > 0.
 */

// This test specifically tests error handling when the feature flag is not on.
// Set sampling rate to -1.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1, featureFlagQueryStats: false},
};
const conn = MongoRunner.runMongod(options);
assert.neq(null, conn, 'failed to start mongod');
const testdb = conn.getDB('test');

var coll = testdb[jsTestName()];
coll.drop();

// Bulk insert documents to reduces roundtrips and make timeout on a slow machine less likely.
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 1; i <= 20; i++) {
    bulk.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
}
assert.commandWorked(bulk.execute());

// Pipeline to read queryStats store should fail without feature flag turned on even though sampling
// rate is > 0.
assert.commandFailedWithCode(
    testdb.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}),
    ErrorCodes.QueryFeatureNotAllowed);

// Pipeline, with a filter, to read queryStats store fails without feature flag turned on even
// though sampling rate is > 0.
assert.commandFailedWithCode(testdb.adminCommand({
    aggregate: 1,
    pipeline: [{$queryStats: {}}, {$match: {"key.queryShape.find": {$eq: "###"}}}],
    cursor: {}
}),
                             ErrorCodes.QueryFeatureNotAllowed);

MongoRunner.stopMongod(conn);
