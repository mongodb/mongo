/**
 * Tests running the delete command on a time-series collection.
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
    assert.commandWorked(insert(coll, {[timeFieldName]: ISODate(), [metaFieldName]: "A"}));

    assert.commandFailedWithCode(coll.remove({tag: "A"}), ErrorCodes.IllegalOperation);

    // Query on a single field that is the metaField.
    assert.commandFailedWithCode(coll.remove({[metaFieldName]: {b: "B"}}),
                                 ErrorCodes.IllegalOperation);

    // Query on a single field that is not the metaField.
    assert.commandFailedWithCode(coll.remove({measurement: "cpu"}), ErrorCodes.InvalidOptions);

    // Query on a single field that is the metaField using dot notation.
    assert.commandFailedWithCode(coll.remove({[metaFieldName + ".a"]: "A"}),
                                 ErrorCodes.IllegalOperation);

    // Query on a single field that is not the metaField using dot notation.
    assert.commandFailedWithCode(coll.remove({"measurement.A": "cpu"}), ErrorCodes.InvalidOptions);

    // Compound query on the metaField using dot notation.
    assert.commandFailedWithCode(
        coll.remove({"$and": [{[metaFieldName + ".b"]: "B"}, {[metaFieldName + ".a"]: "A"}]}),
        ErrorCodes.IllegalOperation);

    // Multiple queries on a single field that is the metaField.
    assert.commandFailedWithCode(testDB.runCommand({
        delete: coll.getName(),
        deletes: [
            {q: {[metaFieldName]: {a: "A", b: "B"}}, limit: 0},
            {q: {"$and": [{[metaFieldName]: {b: "B"}}, {[metaFieldName]: {a: "A"}}]}, limit: 0}
        ]
    }),
                                 ErrorCodes.IllegalOperation);

    // Multiple queries on both the metaField and a field that is not the metaField.
    assert.commandFailedWithCode(testDB.runCommand({
        delete: coll.getName(),
        deletes: [
            {q: {[metaFieldName]: {a: "A", b: "B"}}, limit: 0},
            {q: {measurement: "cpu", [metaFieldName]: {b: "B"}}, limit: 0}
        ],
    }),
                                 ErrorCodes.InvalidOptions);
});
})();
