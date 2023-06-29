/**
 * Tests that timeseries timeField is parsed as bson.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */

(function() {
'use strict';

const collName = "timeseries_field_parsed_as_bson";
const coll = db.getCollection(collName);

coll.drop();
const timeField = "badInput']}}}}}}";
assert.commandWorked(db.createCollection(collName, {timeseries: {timeField: timeField}}));

const timeseriesMaxOptions =
    db.getCollectionInfos({name: "system.buckets." + collName})[0]
        .options.validator.$jsonSchema.properties.control.properties.max.properties;
assert.eq(timeseriesMaxOptions, {"badInput']}}}}}}": {bsonType: 'date'}});

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
})();
