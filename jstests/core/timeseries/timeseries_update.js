/**
 * Tests running the update command on a time-series collection.
 * @tags: [
 *   assumes_no_implicit_collection_creation_after_drop,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   requires_fcv_50,
 *   requires_getmore,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection('t');
    const timeFieldName = "time";
    const metaFieldName = "tag";

    assert.commandWorked(testDB.createCollection(
        coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));

    assert.commandWorked(
        insert(coll, {[timeFieldName]: ISODate(), [metaFieldName]: {a: "A", b: "B"}}));

    // Query on a single field that is the metaField.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName]: {b: "B"}}, {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.IllegalOperation);

    // Query on a single field that is not the metaField.
    assert.commandFailedWithCode(
        coll.update({measurement: "cpu"}, {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.InvalidOptions);

    // Query on both the metaField and a field that is not the metaField.
    assert.commandFailedWithCode(coll.update({measurement: "cpu", [metaFieldName]: {b: "B"}},
                                             {$set: {[metaFieldName]: {b: "C"}}}),
                                 ErrorCodes.InvalidOptions);

    // Compound query on the metaField.
    assert.commandFailedWithCode(
        coll.update({"$and": [{[metaFieldName]: {b: "B"}}, {[metaFieldName]: {a: "A"}}]},
                    {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.IllegalOperation);

    // Compound query on the metaField using dot notation.
    assert.commandFailedWithCode(
        coll.update({"$and": [{[metaFieldName + ".b"]: "B"}, {[metaFieldName + ".a"]: "A"}]},
                    {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.IllegalOperation);

    // Query on a single field that is the metaField.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName]: {a: "A", b: "B"}}, {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.IllegalOperation);

    // Query on a single field that is the metaField using dot notation.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName + ".a"]: "A"}, {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.IllegalOperation);

    // Query on a single field that is not the metaField using dot notation.
    assert.commandFailedWithCode(
        coll.update({"measurement.A": "cpu"}, {$set: {[metaFieldName]: {b: "C"}}}),
        ErrorCodes.InvalidOptions);

    // Multiple queries on a single field that is the metaField.
    assert.commandFailedWithCode(testDB.runCommand({
        update: coll.getName(),
        updates: [
            {q: {[metaFieldName]: {a: "A", b: "B"}}, u: {$set: {[metaFieldName]: {b: "C"}}}},
            {
                q: {"$and": [{[metaFieldName]: {b: "B"}}, {[metaFieldName]: {a: "A"}}]},
                u: {$set: {[metaFieldName]: {b: "C"}}}
            }
        ]
    }),
                                 ErrorCodes.IllegalOperation);

    // Multiple queries on both the metaField and a field that is not the metaField.
    assert.commandFailedWithCode(testDB.runCommand({
        update: coll.getName(),
        updates: [
            {q: {[metaFieldName]: {a: "A", b: "B"}}, u: {$set: {[metaFieldName]: {b: "C"}}}},
            {
                q: {measurement: "cpu", [metaFieldName]: {b: "B"}},
                u: {$set: {[metaFieldName]: {b: "C"}}}
            }
        ]
    }),
                                 ErrorCodes.InvalidOptions);
});
}());
