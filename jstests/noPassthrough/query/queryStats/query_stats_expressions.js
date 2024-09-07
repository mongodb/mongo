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

function makeAggCmd(pipeline, collName = coll.getName()) {
    return {aggregate: collName, pipeline: pipeline, cursor: {}};
}

// Tests that $queryStats stage does not return any entries for pipelines with $getField with
// non-string 'field' argument. These all fail at run-time and so no query stats are collected for
// them.
{
    resetQueryStatsStore(conn, "1MB");
    assert.commandFailedWithCode(
        testDB.runCommand(makeAggCmd([{$project: {a: {$getField: {$add: [1, 2]}}}}])), 3041704);
    assert.commandFailedWithCode(
        testDB.runCommand(makeAggCmd([{$project: {a: {$getField: ["a", "b"]}}}])), 3041704);
    assert.commandFailedWithCode(
        testDB.runCommand(makeAggCmd([{$project: {a: {$getField: null}}}])), 3041704);
    assert.commandFailedWithCode(
        testDB.runCommand(makeAggCmd(
            [{$project: {a: {$getField: {field: {$add: [1, 2]}, input: "$$CURRENT"}}}}])),
        3041704);
    assert.commandFailedWithCode(
        testDB.runCommand(
            makeAggCmd([{$project: {a: {$getField: {field: ["a", "b"], input: "$$CURRENT"}}}}])),
        3041704);
    assert.commandFailedWithCode(
        testDB.runCommand(
            makeAggCmd([{$project: {a: {$getField: {field: null, input: "$$CURRENT"}}}}])),
        3041704);
    let queryStats = getQueryStats(conn);
    assert.eq(queryStats.length, 0, `Expected no entries but got ${tojson(queryStats)}`);
}

// Tests that $queryStats stage does not fail with a re-parse error for a pipeline with $getField
// with non-string 'field' argument. These all have syntax errors for $getField's field argument
// but they do not fail because there's no collection named "nocoll" and so they make it to the
// query stats store. $queryStats should be able to retrive them without any re-parse error.
{
    resetQueryStatsStore(conn, "1MB");

    // query shape #1
    testDB.runCommand(makeAggCmd([{$project: {a: {$getField: {$add: [1, 2]}}}}], "nocoll"));
    testDB.runCommand(makeAggCmd(
        [{$project: {a: {$getField: {field: {$add: [1, 2]}, input: "$$CURRENT"}}}}], "nocoll"));

    // query shape #2
    testDB.runCommand(makeAggCmd([{$project: {a: {$getField: ["a", "b"]}}}], "nocoll"));
    testDB.runCommand(makeAggCmd(
        [{$project: {a: {$getField: {field: ["a", "b"], input: "$$CURRENT"}}}}], "nocoll"));

    // query shape #3
    testDB.runCommand(makeAggCmd([{$project: {a: {$getField: null}}}], "nocoll"));
    testDB.runCommand(
        makeAggCmd([{$project: {a: {$getField: {field: null, input: "$$CURRENT"}}}}], "nocoll"));

    let queryStats = getQueryStats(conn);
    assert.eq(queryStats.length, 3, `Expected 3 entries but got ${tojson(queryStats)}`);
}

// Test re-parsing for $expressionN expression types (e.g. $maxN) where the 'n' expression evaluates
// differently from original shape to representative shape.
{
    resetQueryStatsStore(conn, "1MB");

    // Query shape with $maxN.
    testDB.runCommand(makeAggCmd([{
        $setWindowFields: {
            output: {
                max: {
                    $maxN: {
                        n: {$abs: {$ceil: {$strcasecmp: ["Grocery Small", "withdrawal"]}}},
                        input: "$x"
                    }
                }
            }
        }
    }]));

    // Query shape with $minN.
    testDB.runCommand(makeAggCmd([{
        $setWindowFields: {
            output: {
                max: {
                    $minN: {
                        n: {$abs: {$ceil: {$strcasecmp: ["Grocery Small", "withdrawal"]}}},
                        input: "$x"
                    }
                }
            }
        }
    }]));

    const queryStats = getQueryStats(conn);
    assert.eq(queryStats.length, 2, `Expected 2 entries but got ${tojson(queryStats)}`);
}

MongoRunner.stopMongod(conn);
