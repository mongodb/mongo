/**
 * Test the $queryStats hmac properties.
 * @tags: [featureFlagQueryStats]
 */

load("jstests/aggregation/extras/utils.js");  // For assertAdminDBErrCodeAndErrMsgContains.
load("jstests/libs/query_stats_utils.js");

(function() {
"use strict";

// Assert the expected queryStats key with no hmac.
function assertQueryStatsKeyWithoutHmac(queryStatsKey) {
    assert.eq(queryStatsKey.filter, {"foo": {"$lte": "?number"}});
    assert.eq(queryStatsKey.sort, {"bar": -1});
    assert.eq(queryStatsKey.limit, "?number");
}

function runTest(conn) {
    const testDB = conn.getDB('test');
    var coll = testDB[jsTestName()];
    coll.drop();

    coll.insert({foo: 1});
    coll.find({foo: {$lte: 2}}).sort({bar: -1}).limit(2).toArray();
    // Default is no hmac.
    let ret = getQueryStatsFindCmd(conn)[0].key.queryShape;
    assertQueryStatsKeyWithoutHmac(getQueryStatsFindCmd(conn)[0].key.queryShape);

    // Turning on hmac should apply hmac to all field names on all entries, even previously cached
    // ones.
    const queryStatsKey = getQueryStatsFindCmd(conn, {transformIdentifiers: true})[0]["key"];
    assert.eq(queryStatsKey.queryShape.filter,
              {"fNWkKfogMv6MJ77LpBcuPrO7Nq+R+7TqtD+Lgu3Umc4=": {"$lte": "?number"}});
    assert.eq(queryStatsKey.queryShape.sort, {"CDDQIXZmDehLKmQcRxtdOQjMqoNqfI2nGt2r4CgJ52o=": -1});
    assert.eq(queryStatsKey.queryShape.limit, "?number");

    // Turning hmac back off should preserve field names on all entries, even previously cached
    // ones.
    const queryStats = getQueryStats(conn)[1]["key"];
    assertQueryStatsKeyWithoutHmac(queryStats.queryShape);

    // Explicitly set transformIdentifiers to false.
    assertQueryStatsKeyWithoutHmac(
        getQueryStatsFindCmd(conn, {transformIdentifiers: false})[0]["key"].queryShape);

    // Wrong parameter name throws error.
    let pipeline = [{$queryStats: {redactFields: true}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats parameters object may only contain 'transformIdentifiers'. Found: redactFields");

    // Wrong parameter name throws error.
    pipeline = [{$queryStats: {algorithm: "hmac-sha-256"}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats parameters object may only contain 'transformIdentifiers'. Found: algorithm");

    // Wrong parameter type throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: 1}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats algorithm parameter must be a string. Found type: double");

    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256", hmacKey: 1}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats hmacKey parameter must be bindata of length 32 or greater. Found type: double");

    // Unsupported algorithm throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-1"}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats algorithm currently supported is only 'hmac-sha-256'. Found: hmac-sha-1");

    // TransformIdentifiers with missing algorithm throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats missing value for algorithm, which is required for 'transformIdentifiers'");

    // Parameter object with unrecognized key throws error.
    pipeline =
        [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256", hmacStrategy: "on"}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "$queryStats parameters to 'transformIdentifiers' may only contain 'algorithm' or 'hmacKey' options. Found: hmacStrategy");
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
        featureFlagQueryStats: true,
    }
});
runTest(conn);
MongoRunner.stopMongod(conn);

const st = new ShardingTest({
    mongos: 1,
    shards: 1,
    config: 1,
    rs: {nodes: 1},
    mongosOptions: {
        setParameter: {
            internalQueryStatsRateLimit: -1,
            'failpoint.skipClusterParameterRefresh': "{'mode':'alwaysOn'}"
        }
    },
});
runTest(st.s);
st.stop();
}());
