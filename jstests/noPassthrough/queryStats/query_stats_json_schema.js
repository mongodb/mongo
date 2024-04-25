/**
 * Test that $queryStats properly tokenizes jsonSchema queries with a "properties" keyword, on
 * mongod and mongos.
 * @tags: [requires_fcv_81]
 */
import {getQueryStatsAggCmd, kShellApplicationName} from "jstests/libs/query_stats_utils.js";

const kHashedDbName = "iDlS7h5jf5HHxWPJpeHRbA+jLTNNZaqxVVkplrEkfko=";
const kHashedCollName = "w6Ax20mVkbJu4wQWAMjL8Sl+DfXAr2Zqdc3kJRB7Oo0=";

function verifyConsistentFields(key) {
    assert.eq({"db": `${kHashedDbName}`, "coll": `${kHashedCollName}`}, key.queryShape.cmdNs);
    assert.eq("aggregate", key.queryShape.command);
    assert.eq(kShellApplicationName, key.client.application.name);
}

function runAndVerifyQueryStatsTokenization(
    coll, admin, pipeline, expectedResult, statsSize, index) {
    coll.aggregate(pipeline).toArray();
    const stats = getQueryStatsAggCmd(admin, {transformIdentifiers: true});

    assert.eq(statsSize, stats.length);
    const key = stats[index].key;
    verifyConsistentFields(key);
    // Make sure there is no otherNss field when there are no secondary namespaces.
    assert(!key.hasOwnProperty('otherNss'), key);
    assert.eq(expectedResult, key.queryShape.pipeline, key.queryShape.pipeline);
}

// Checks proper tokenization on a pipeline with a $jsonSchema match stage, which undergoes
// reparsing. Validate that the dollar field is permitted in the BSONObj upon reparsing.
function runTest(conn) {
    const db = conn.getDB("testDB");
    const admin = conn.getDB("admin");

    const coll = db.test;
    const otherColl = db.otherColl;

    coll.drop();
    otherColl.drop();
    assert.commandWorked(coll.insert({a: "foobar", b: 15}));
    assert.commandWorked(coll.insert({a: "foobar", b: 20}));
    assert.commandWorked(otherColl.insert({a: "foobar", price: 2.50}));

    var statsSize = 0;

    // jsonSchema with $stdDevPop field and $_internalSchemaType is correctly tokenized.
    // Reproduces BF-31616.
    {
        const kHashedDollarFieldName = "TQCdAEcs07vAzO0JeYUN70ULbcoe4rVOR6wzaUZXpjo=";
        const pipeline = [{$match: {$jsonSchema: {properties: {$stdDevPop: {type: 'array'}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {[kHashedDollarFieldName]: {"$not": {"$exists": "?bool"}}},
                            {"$and": [{[kHashedDollarFieldName]: {"$_internalSchemaType": [4]}}]}
                        ]
                    }]
                }]
            }
        }];
        statsSize += 1;
        const index = 0;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    // jsonSchema with $slice field and $_internalSchemaType is correctly tokenized.
    // Reproduces BF-31947.
    {
        const kHashedDollarFieldName = "8+OxL+R4EJUT2/8luDRlK+3ZDQ3kPD6h7gscG44OQtw=";
        const pipeline = [{$match: {$jsonSchema: {properties: {$slice: {type: 'number'}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {[kHashedDollarFieldName]: {"$not": {"$exists": "?bool"}}},
                            {
                                "$and": [
                                    {[kHashedDollarFieldName]: {"$_internalSchemaType": ["number"]}}
                                ]
                            }
                        ]
                    }]
                }]
            }
        }];
        statsSize += 1;
        const index = 0;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    // jsonSchema with $bitsAnySet field, $_internalSchemaType, and $_internalSchemaMinLength is
    // correctly tokenized. Reproduces BF-32040.
    {
        const kHashedDollarFieldName = "48mlorj6MWkqJLHiyvv/5h1Doa+b8Pi7C3hH8O48Y5A=";
        const pipeline =
            [{$match: {$jsonSchema: {properties: {$bitsAnySet: {type: 'string', minLength: 6}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {[kHashedDollarFieldName]: {"$not": {"$exists": "?bool"}}},
                            {
                                "$and": [
                                    {
                                        [kHashedDollarFieldName]:
                                            {"$_internalSchemaMinLength": "?number"}
                                    },
                                    {[kHashedDollarFieldName]: {"$_internalSchemaType": [2]}}
                                ]
                            }
                        ]
                    }]
                }]
            }
        }];
        statsSize += 1;
        const index = 0;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    // jsonSchema with $or (special MQL keyword) field and $_internalSchemaType is correctly
    // tokenized. Reproduces BF-32434.
    {
        const kHashedDollarFieldName = "gkp/+o9pv4jAJ5BBli/hmBNhOIyDgqBdEzcvDEsOl10=";
        const kHashedProjectFieldName = "+0wgDp/AI7f+XT+DJEqixDyZBq9zRe7RGN0wCS9bd94=";
        const kHashedSortFieldName = "f1y+Zd+zftcL7s1T18//NJHdkoaUBsRvMybBs7v5BeQ=";

        const pipeline = [
            {$match: {$jsonSchema: {properties: {$or: {bsonType: 'long'}}}}},
            {$project: {_id: 1}},
            {$sort: {getName: 1}}
        ];
        const expectedResult = [
            {
                "$match": {
                    "$and": [{
                        "$and": [{
                            "$or": [
                                {[kHashedDollarFieldName]: {"$not": {"$exists": "?bool"}}},
                                {
                                    "$and":
                                        [{[kHashedDollarFieldName]: {"$_internalSchemaType": [18]}}]
                                }
                            ]
                        }]
                    }]
                },
            },
            {"$project": {[kHashedProjectFieldName]: true}},
            {"$sort": {[kHashedSortFieldName]: 1}}
        ];
        statsSize += 1;
        // This one will sort last because of the 'project' and 'sort' parameters.
        const index = 3;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    /*
     * Currently, jsonSchema with the "required" keyword and dollar fields is not properly tokenized
     * because the translated MatchExpression doesn't contain a field starting with
     * "$_internalSchema...".
     * TODO SERVER-89844: Make $jsonSchema with dollar fields in all keyword fields reparseable.
    {
        const kHashedArrayName = "mf6JpZnbHDm4Wa6EzBkQ4utXN7IsWMLJvIq0wP4qN1U=";
        const internalSchemaCondSentinel = {
            "$_internalSchemaCond":
                [{"$alwaysTrue": "?number"}, {"$alwaysTrue": "?number"}, {"$alwaysTrue": "?number"}]
        };

        const pipeline = [{$match: {$jsonSchema: {required: ["a", "b", "$dollarPrefixes"]}}}];
        const expectedResult = [{
            "$match": {
                "$or":
                    [{[kHashedArrayName]: {"$eq": "?array<?object>"}}, internalSchemaCondSentinel]
            },
        }];
        statsSize += 1;
        const index = 0;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
     */
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
