/**
 * Test that queryStats works properly for a find command that uses agg expressions and produces the
 * proper query shape without issues during re-parsing.
 * @tags: [requires_fcv_71]
 */
import {getQueryStats, resetQueryStatsStore} from "jstests/libs/query_stats_utils.js";

// Turn on the collecting of queryStats metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB('test');
var coll = testDB[jsTestName()];
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
const numDocs = 100;
for (let i = 0; i < numDocs / 2; ++i) {
    bulk.insert({foo: i, bar: i, applyDiscount: true, word: "asdf"});
    bulk.insert({foo: i, bar: i, applyDiscount: false, word: "ghjk"});
}
assert.commandWorked(bulk.execute());
coll.createIndex({foo: 1});

// Verifies that $getField with a syntax error fails to be parsed and does not make it to the query
// stats store.
(function testGetFieldWithSyntaxError() {
    // If $getField's field argument is a constant, it should be a string. Otherwise, it should fail
    // to be parsed. But if it isn't a string constant, this condition was not correctly checked and
    // it made it to the query stats store and was causing an re-parsing error for $queryStats
    // pipeline stage execution.
    assert.throwsWithCode(
        () => testDB.nocoll.aggregate([{$project: {a: {$getField: [[]]}}}]).toArray(), 5654602);
    assert.throwsWithCode(
        () => testDB.nocoll.aggregate([{$project: {a: {$getField: ["a"]}}}]).toArray(), 5654602);
    assert.throwsWithCode(
        () => testDB.nocoll.aggregate([{$project: {a: {$getField: [1, 2, 3]}}}]).toArray(),
        5654602);
    assert.throwsWithCode(
        () => testDB.nocoll.aggregate([{$project: {a: {$getField: [{b: "c"}]}}}]).toArray(),
        5654602);
    assert.throwsWithCode(
        () => testDB.nocoll
                  .aggregate([{$project: {a: {$getField: {field: [[]], input: "$$CURRENT"}}}}])
                  .toArray(),
        5654602);
    assert.throwsWithCode(
        () => testDB.nocoll
                  .aggregate([{$project: {a: {$getField: {field: ["a"], input: "$$CURRENT"}}}}])
                  .toArray(),
        5654602);
    assert.throwsWithCode(
        () => testDB.nocoll
                  .aggregate([{$project: {a: {$getField: {field: [1, 2, 3], input: "$$CURRENT"}}}}])
                  .toArray(),
        5654602);
    assert.throwsWithCode(
        () =>
            testDB.nocoll
                .aggregate([{$project: {a: {$getField: {field: [{b: "c"}], input: "$$CURRENT"}}}}])
                .toArray(),
        5654602);
    let queryStats = getQueryStats(testDB);
    assert.eq(queryStats.length, 0, `Expected 0 elements but got ${tojson(queryStats)}`);
})();

// Tests that $meta is re-parsed correctly by ensuring the metaDataKeyword is not serialized as
// string literal.
{
    resetQueryStatsStore(conn, "1MB");
    coll.find({$expr: {idxKey: {$meta: "indexKey"}}}).itcount();
    let queryStats = getQueryStats(testDB);
    assert.eq({"$expr": {"idxKey": {"$meta": "indexKey"}}}, queryStats[0].key.queryShape.filter);
}

// Tests that $regexMatch is re-parsed correctly. The parser does not check that regex is a valid
// regex pattern, so regular serialization is okay.
{
    resetQueryStatsStore(conn, "1MB");
    coll.find({$expr: {result: {$regexMatch: {input: "$word", regex: "^a"}}}}).itcount();
    let queryStats = getQueryStats(testDB);
    assert.eq({"$expr": {"result": {"$regexMatch": {"input": "$word", "regex": "?string"}}}},
              queryStats[0].key.queryShape.filter);
}

// Tests that $let is re-parsed correctly, by ensuring the variables are not serialized as string
// literals.
{
    resetQueryStatsStore(conn, "1MB");
    coll.find({$expr: {$let: {
       vars: {
          total: { $add: [ '$foo', '$bar' ] },
          discounted: { $cond: { if: '$applyDiscount', then: 0.9, else: 1 } }
       },
       in: { $multiply: [ "$$total", "$$discounted" ] }
    }}}).itcount();
    let queryStats = getQueryStats(testDB);
    assert.eq({
        "$expr": {
            "$let": {
                "vars": {
                    "total": {"$add": ["$foo", "$bar"]},
                    "discounted": {"$cond": ["$applyDiscount", "?number", "?number"]}
                },
                "in": {"$multiply": ["$$total", "$$discounted"]}
            }
        }
    },
              queryStats[0].key.queryShape.filter);
}

MongoRunner.stopMongod(conn);
