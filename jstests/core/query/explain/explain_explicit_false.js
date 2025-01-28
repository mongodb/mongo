// Tests that an aggregation command works with explain explicitly set to false.

const testDB = db.getSiblingDB("testDB");
assert.commandWorked(testDB.dropDatabase());
const collName = jsTestName() + "_coll";
const coll = testDB.getCollection(collName);
const ns = "testDB." + collName;

const documents = [{a: 3, b: 3}, {a: 4, b: 1}, {a: 4, b: 2}, {a: 4, b: 3}, {a: 2, b: 3}];
assert.commandWorked(coll.insertMany(documents));

const pipeline = [
    {$match: {a: {$gte: 3}}},
    {$group: {_id: {}, bSum: {$sum: "$b"}}},
    {$addFields: {newField: "test"}}
];

const result = assert.commandWorked(
    testDB.runCommand({aggregate: coll.getName(), pipeline: pipeline, cursor: {}, explain: false}));

assert.eq(result.ok, 1);
assert.eq([{"_id": {}, "bSum": 9, "newField": "test"}], result.cursor.firstBatch);
assert.eq(ns, result.cursor.ns);
