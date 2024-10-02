/**
 * Test the $queryStats hmac properties.
 * @tags: [requires_fcv_71]
 */

import {assertAdminDBErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {getQueryStatsFindCmd} from "jstests/libs/query/query_stats_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Assert the expected queryStats key with no hmac.
function assertQueryStatsKeyWithoutHmac(queryStatsKey) {
    assert.eq(queryStatsKey.filter, {"foo": {"$lte": "?number"}});
    assert.eq(queryStatsKey.sort, {"bar": -1});
    assert.eq(queryStatsKey.limit, "?number");
}

function runTest(conn) {
    const testDB = conn.getDB('test');
    const coll = testDB[jsTestName()];
    coll.drop();

    coll.insert({foo: 1});
    coll.find({foo: {$lte: 2}}).sort({bar: -1}).limit(2).toArray();
    // Default is no hmac.
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
    const queryStats = getQueryStatsFindCmd(conn)[0]["key"];
    assertQueryStatsKeyWithoutHmac(queryStats.queryShape);

    // Explicitly set transformIdentifiers to false.
    assertQueryStatsKeyWithoutHmac(
        getQueryStatsFindCmd(conn, {transformIdentifiers: false})[0]["key"].queryShape);

    // Wrong parameter name throws error.
    let pipeline = [{$queryStats: {redactFields: true}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.IDLUnknownField,
        "BSON field '$queryStats.redactFields' is an unknown field.");

    // Wrong parameter name throws error.
    pipeline = [{$queryStats: {algorithm: "hmac-sha-256"}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.IDLUnknownField,
        "BSON field '$queryStats.algorithm' is an unknown field.");

    // Wrong parameter type throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: 1}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.TypeMismatch,
        "BSON field '$queryStats.transformIdentifiers.algorithm' is the wrong type 'double', expected type 'string'");

    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256", hmacKey: 1}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.TypeMismatch,
        "BSON field '$queryStats.transformIdentifiers.hmacKey' is the wrong type 'double', expected type 'binData'");

    // Unsupported algorithm throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-1"}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.BadValue,
        "Enumeration value 'hmac-sha-1' for field '$queryStats.transformIdentifiers.algorithm' is not a valid value.");

    // TransformIdentifiers with missing algorithm throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.IDLFailedToParse,
        "BSON field '$queryStats.transformIdentifiers.algorithm' is missing but a required field");

    // TransformIdentifiers with algorithm but missing hmacKey throws error.
    pipeline = [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256"}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.FailedToParse,
        "The 'hmacKey' parameter of the $queryStats stage must be specified when applying the hmac-sha-256 algorithm");

    // Parameter object with unrecognized key throws error.
    pipeline =
        [{$queryStats: {transformIdentifiers: {algorithm: "hmac-sha-256", hmacStrategy: "on"}}}];
    assertAdminDBErrCodeAndErrMsgContains(
        coll,
        pipeline,
        ErrorCodes.IDLUnknownField,
        "BSON field '$queryStats.transformIdentifiers.hmacStrategy' is an unknown field.");
}

const conn = MongoRunner.runMongod({
    setParameter: {
        internalQueryStatsRateLimit: -1,
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
