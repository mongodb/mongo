/**
 * Test that query stats key generation works for queries with non-object fields.
 * @tags: [requires_fcv_72]
 */
// Turn on the collecting of query stats metrics.
let options = {
    setParameter: {internalQueryStatsRateLimit: -1},
};

const conn = MongoRunner.runMongod(options);
const testDB = conn.getDB("test");
let collA = testDB[jsTestName()];
let collB = testDB[jsTestName() + "Two"];
collA.drop();
collB.drop();

for (let i = 0; i < 200; i++) {
    collA.insert({foo: 0, bar: Math.floor(Math.random() * 3)});
    collA.insert({foo: 1, bar: Math.floor(Math.random() * -2)});
    collB.insert({foo: Math.floor(Math.random() * 2), bar: Math.floor(Math.random() * 2)});
}

function confirmAggSuccess(collName, pipeline) {
    const command = {aggregate: collName, cursor: {}};
    command.pipeline = pipeline;
    assert.commandWorked(testDB.runCommand(command));
}
// Test with non-object fields $limit and $skip.
confirmAggSuccess(collA.getName(), [{$sort: {bar: -1}}, {$limit: 2}, {$match: {foo: {$lte: 2}}}]);
confirmAggSuccess(collA.getName(), [{$sort: {bar: -1}}, {$skip: 50}, {$match: {foo: {$lte: 2}}}]);
confirmAggSuccess(collA.getName(), [{$sort: {bar: -1}}, {$limit: 2}, {$skip: 50}, {$match: {foo: 0}}]);

// Test non-object field, $unionWith.
confirmAggSuccess(collA.getName(), [{$unionWith: collB.getName()}]);

// Test $limit in $setWindowFields for good measure.
confirmAggSuccess(collA.getName(), [
    {$_internalInhibitOptimization: {}},
    {
        $setWindowFields: {
            sortBy: {foo: 1},
            output: {sum: {$sum: "$bar", window: {documents: ["unbounded", "current"]}}},
        },
    },
    {$sort: {foo: 1}},
    {$limit: 5},
]);
// Test find commands containing non-object fields
assert.commandWorked(testDB.runCommand({find: collA.getName(), limit: 20}));
assert.commandWorked(testDB.runCommand({find: collA.getName(), skip: 199}));
collA.find().skip(100);

// findOne has a nonobject field, $limit.
collB.findOne();
collB.findOne({foo: 1});

// Test non-object field $unwind
confirmAggSuccess(collA.getName(), [
    {
        "$facet": {
            "productOfJoin": [
                {"$lookup": {"from": collB.getName(), "pipeline": [{"$match": {}}], "as": "join"}},
                {"$unwind": "$join"},
                {"$project": {"str": 1}},
            ],
        },
    },
]);

MongoRunner.stopMongod(conn);
