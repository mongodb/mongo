/**
 * Tests that timeseries timeField is parsed as bson.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_71,
 * ]
 */

const collName = jsTestName();
const coll = db.getCollection(collName);

coll.drop();
const timeField = "badInput']}}}}}}";
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: timeField}}));

const timeseriesCollInfo = db.getCollectionInfos({name: "system.buckets." + collName})[0];
jsTestLog("Timeseries system collection info: " + tojson(timeseriesCollInfo));
const properties = {};
properties[timeField] = {
    "bsonType": "date"
};
const expectedValidator = {
    "$jsonSchema": {
        "bsonType": "object",
        "required": ["_id", "control", "data"],
        "properties": {
            "_id": {"bsonType": "objectId"},
            "control": {
                "bsonType": "object",
                "required": ["version", "min", "max"],
                "properties": {
                    "version": {"bsonType": "number"},
                    "min":
                        {"bsonType": "object", "required": [timeField], "properties": properties},
                    "max":
                        {"bsonType": "object", "required": [timeField], "properties": properties},
                    "closed": {"bsonType": "bool"},
                    "count": {"bsonType": "number", "minimum": 1}
                },
                "additionalProperties": false
            },
            "data": {"bsonType": "object"},
            "meta": {}
        },
        "additionalProperties": false
    }
};

assert(timeseriesCollInfo.options);
assert.eq(timeseriesCollInfo.options.validator, expectedValidator);

const doc = {
    a: 1,
    [timeField]: new Date("2021-01-01")
};
assert.commandWorked(coll.insert(doc));
assert.docEq([doc], coll.aggregate([{$match: {}}, {$project: {_id: 0}}]).toArray());

coll.drop();
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "\\"}}));
coll.drop();
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: "\\\\"}}));