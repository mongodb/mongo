/**
 * Tests that validate will detect a compressed bucket with time out-of-order.
 */
const conn = MongoRunner.runMongod();

const dbName = jsTestName();
const collName = 'ts';
const testDB = conn.getDB(dbName);
const tsColl = testDB[collName];
const bucketsColl = testDB.getCollection('system.buckets.' + collName);

const timeField = 't';
assert.commandWorked(testDB.createCollection(collName, {timeseries: {timeField: timeField}}));

// Compressed bucket with the compressed time field out-of-order.
assert.commandWorked(bucketsColl.insert({
    "_id": ObjectId("630ea4802093f9983fc394dc"),
    "control": {
        "version": NumberInt(2),
        "min": {
            "_id": ObjectId("630fabf7c388456f8aea4f2d"),
            "t": ISODate("2022-08-31T00:00:00.000Z"),
            "a": 0
        },
        "max": {
            "_id": ObjectId("630fabf7c388456f8aea4f2f"),
            "t": ISODate("2022-08-31T00:00:01.000Z"),
            "a": 1
        },
        "count": 2
    },
    "data": {
        "t": BinData(7, "CQDolzLxggEAAID+fAAAAAAAAAA="),
        "_id": BinData(7, "BwBjD6v3w4hFb4rqTy2ATgAAAAAAAAAA"),
        "a": BinData(7, "EAAAAAAAgC4AAAAAAAAAAA==")
    }
}));

let res = assert.commandWorked(tsColl.validate({full: true}));
assert(!res.valid);
assert.eq(res.errors.length, 1);

MongoRunner.stopMongod(conn, null, {skipValidation: true});
