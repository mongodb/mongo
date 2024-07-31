/**
 * Test that $queryStats properly tokenizes $jsonSchema queries with dollar-prefixed field names on
 * mongod and mongos.
 */
(function() {
'use strict';

load("jstests/libs/query_stats_utils.js");  // For getQueryStatsAggCmd and kShellApplicationName.

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

    // $jsonSchema properties list with $stdDevPop field and $_internalSchemaType is correctly
    // tokenized. Reproduces BF-31616.
    {
        const kHashedDollarFieldName = "TQCdAEcs07vAzO0JeYUN70ULbcoe4rVOR6wzaUZXpjo=";
        const pipeline = [{$match: {$jsonSchema: {properties: {$stdDevPop: {type: 'array'}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {
                                "$nor": [{
                                    "$_internalPath":
                                        {[kHashedDollarFieldName]: {"$exists": "?bool"}}
                                }]
                            },
                            {
                                "$and": [{
                                    "$_internalPath":
                                        {[kHashedDollarFieldName]: {"$_internalSchemaType": [4]}}
                                }]
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
    // $jsonSchema properties list with $slice field and $_internalSchemaType is correctly
    // tokenized. Reproduces BF-31947.
    {
        const kHashedDollarFieldName = "8+OxL+R4EJUT2/8luDRlK+3ZDQ3kPD6h7gscG44OQtw=";
        const pipeline = [{$match: {$jsonSchema: {properties: {$slice: {type: 'number'}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {
                                "$nor": [{
                                    "$_internalPath":
                                        {[kHashedDollarFieldName]: {"$exists": "?bool"}}
                                }]
                            },
                            {
                                "$and": [{
                                    "$_internalPath": {
                                        [kHashedDollarFieldName]:
                                            {"$_internalSchemaType": ["number"]}
                                    }
                                }]
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
    // $jsonSchema properties list with $bitsAnySet field, $_internalSchemaType, and
    // $_internalSchemaMinLength is correctly tokenized. Reproduces BF-32040.
    {
        const kHashedDollarFieldName = "48mlorj6MWkqJLHiyvv/5h1Doa+b8Pi7C3hH8O48Y5A=";
        const pipeline =
            [{$match: {$jsonSchema: {properties: {$bitsAnySet: {type: 'string', minLength: 6}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {
                                "$nor": [{
                                    "$_internalPath":
                                        {[kHashedDollarFieldName]: {"$exists": "?bool"}}
                                }]
                            },
                            {
                                "$and": [
                                    {
                                        "$_internalPath": {
                                            [kHashedDollarFieldName]:
                                                {"$_internalSchemaMinLength": "?number"}
                                        },
                                    },
                                    {
                                        "$_internalPath": {
                                            [kHashedDollarFieldName]: {"$_internalSchemaType": [2]}
                                        },
                                    }
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
    // $jsonSchema properties list with empty $slice field is correctly tokenized.
    // Reproduces BF-33441.
    {
        const kHashedDollarFieldName = "8+OxL+R4EJUT2/8luDRlK+3ZDQ3kPD6h7gscG44OQtw=";

        const pipeline = [{$match: {$jsonSchema: {properties: {$slice: {}}}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [{
                        "$or": [
                            {
                                "$nor": [{
                                    "$_internalPath":
                                        {[kHashedDollarFieldName]: {"$exists": "?bool"}}
                                }]
                            },
                            {}
                        ]
                    }]
                }]
            }
        }];
        statsSize += 1;
        const index = 1;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    // $jsonSchema properties list with $or (special MQL keyword) field and $_internalSchemaType is
    // correctly tokenized. Reproduces BF-32434.
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
                                {
                                    "$nor": [{
                                        "$_internalPath":
                                            {[kHashedDollarFieldName]: {"$exists": "?bool"}}
                                    }]
                                },
                                {
                                    "$and": [{
                                        "$_internalPath": {
                                            [kHashedDollarFieldName]: {"$_internalSchemaType": [18]}
                                        }
                                    }]
                                }
                            ]
                        }]
                    }]
                }
            },
            {"$project": {[kHashedProjectFieldName]: true}},
            {"$sort": {[kHashedSortFieldName]: 1}}
        ];
        statsSize += 1;
        // This one will sort last because of the 'project' and 'sort' parameters.
        const index = 4;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    // $jsonSchema with a "required" keyword and dollar fields is properly tokenized. Reproduces
    // BF-33441.
    {
        const kHashedDollarFieldName = "I8dnZuPUk2wDamTSSL481i1eTea4aLrPj0pLA8+T9uM=";
        const kHashedFieldA = "GDiF6ZEXkeo4kbKyKEAAViZ+2RHIVxBQV9S6b6Lu7gU=";
        const kHashedFieldB = "m1xtUkfSpZNxXjNZYKwo86vGD37Zxmd2gtt+TXDO558=";

        const pipeline = [{$match: {$jsonSchema: {required: ["a", "b", "$dollarPrefixes"]}}}];
        const expectedResult = [{
            "$match": {
                "$and": [{
                    "$and": [
                        {"$_internalPath": {[kHashedDollarFieldName]: {"$exists": "?bool"}}},
                        {[kHashedFieldA]: {"$exists": "?bool"}},
                        {[kHashedFieldB]: {"$exists": "?bool"}}
                    ]
                }]
            }
        }];
        statsSize += 1;
        const index = 0;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
    // $jsonSchema with multiple dollar-prefixed field names in its properties and required lists is
    // properly tokenized.
    {
        const kHashedSliceName = "8+OxL+R4EJUT2/8luDRlK+3ZDQ3kPD6h7gscG44OQtw=";
        const kHashedOrName = "gkp/+o9pv4jAJ5BBli/hmBNhOIyDgqBdEzcvDEsOl10=";

        const pipeline = [{
            $match: {
                $jsonSchema: {
                    required: ["$slice", "$or"],
                    properties: {$slice: {type: "number"}, $or: {type: "string"}}
                }
            }
        }];
        const expectedResult = [{
            "$match": {
                "$and": [
                    {
                        "$and": [
                            {
                                "$and": [{
                                    "$_internalPath":
                                        {[kHashedSliceName]: {"$_internalSchemaType": ["number"]}}
                                }]
                            },
                            {
                                "$and": [{
                                    "$_internalPath":
                                        {[kHashedOrName]: {"$_internalSchemaType": [2]}}
                                }]
                            }
                        ]
                    },
                    {
                        "$and": [
                            {"$_internalPath": {[kHashedOrName]: {"$exists": "?bool"}}},
                            {"$_internalPath": {[kHashedSliceName]: {"$exists": "?bool"}}}
                        ]
                    }
                ]
            }
        }];
        statsSize += 1;
        const index = 1;

        runAndVerifyQueryStatsTokenization(coll, admin, pipeline, expectedResult, statsSize, index);
    }
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
    mongosOptions: {setParameter: {internalQueryStatsRateLimit: -1}},
});
runTest(st.s);
st.stop();
})();
