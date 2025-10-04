/**
 * Tests that a time-series collection rejects documents with invalid timeField values
 */
const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const testDB = conn.getDB(dbName);
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection("t");
coll.drop();

const timeFieldName = "time";
const metaFieldName = "m";

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
);

// first test a good doc just in case
const goodDocs = [
    {
        _id: 0,
        time: ISODate("2020-11-26T00:00:00.000Z"),
        [metaFieldName]: "A",
        data: true,
    },
    {
        _id: 1,
        time: ISODate("2020-11-27T00:00:00.000Z"),
        [metaFieldName]: "A",
        data: true,
    },
];
assert.commandWorked(coll.insert(goodDocs[0]));
assert.eq(1, coll.count());
assert.docEq([goodDocs[0]], coll.find().toArray());

// now make sure we reject if timeField is missing or isn't a valid BSON datetime
let mixedDocs = [{[metaFieldName]: "B", data: true}, goodDocs[1], {time: "invalid", [metaFieldName]: "B", data: false}];
assert.commandFailedWithCode(coll.insert(mixedDocs, {ordered: false}), ErrorCodes.BadValue);
assert.eq(coll.count(), 2);
assert.docEq(goodDocs, coll.find().toArray());
assert.eq(null, coll.findOne({[metaFieldName]: mixedDocs[0].m}));
assert.eq(null, coll.findOne({[metaFieldName]: mixedDocs[2].m}));

MongoRunner.stopMongod(conn);
