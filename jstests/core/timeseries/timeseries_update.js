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

if (!TimeseriesTest.timeseriesUpdatesAndDeletesEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series updates and deletes feature flag is disabled");
    return;
}

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

    assert.commandWorked(insert(coll, {
        [timeFieldName]: ISODate(),
        [metaFieldName]: {c: "C", d: 2},
        f1: [{"k": "K", "v": "V"}],
        f2: 0,
        f3: "f3",
    }));

    /*************************** Tests updating with an update document ***************************/
    // Modify a field that is the metaField.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName]: {c: "C", d: 2}}, {$set: {[metaFieldName]: {e: "E"}}}),
        ErrorCodes.IllegalOperation);

    // Modify a field that is not the metaField.
    assert.commandFailedWithCode(coll.update({[metaFieldName]: {c: "C", d: 2}}, {$set: {f2: "f2"}}),
                                 ErrorCodes.InvalidOptions);

    // Modify the metafield and fields that are not the metaField.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName]: {c: "C", d: 2}},
                    {$set: {[metaFieldName]: {e: "E"}, f3: "f3"}, $inc: {f2: 3}, $unset: {f1: ""}}),
        ErrorCodes.InvalidOptions);

    // Modify a field that is the metaField using dot notation.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName + ".c"]: "C"}, {$inc: {[metaFieldName + ".d"]: 10}}),
        ErrorCodes.IllegalOperation);

    // Modify the metaField multiple times.
    assert.commandFailedWithCode(testDB.runCommand({
        update: coll.getName(),
        updates: [
            {q: {[metaFieldName]: {c: "C", d: 2}}, u: {$set: {[metaFieldName]: 1}}},
            {q: {[metaFieldName]: 1}, u: {$set: {[metaFieldName]: 2}}},
            {q: {[metaFieldName]: 2}, u: {$set: {[metaFieldName]: 3}}}
        ]
    }),
                                 ErrorCodes.IllegalOperation);

    // Modify the metaField and a field that is not the metaField using dot notation.
    assert.commandFailedWithCode(testDB.runCommand({
        update: coll.getName(),
        updates: [
            {q: {[metaFieldName]: {c: "C", d: 2}}, u: {$inc: {[metaFieldName + ".d"]: 6}}},
            {q: {[metaFieldName]: {c: "C", d: 8}}, u: {$set: {"f1.0": "f2"}}}
        ]
    }),
                                 ErrorCodes.InvalidOptions);

    /*************************** Tests updating with an update pipeline ***************************/
    // Modify the metaField using dot notation: Add embedded fields to the metaField and remove an
    // embedded field.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName + ".c"]: "C"},
                    [
                        {$set: {[metaFieldName + ".e"]: "E", [metaFieldName + ".f"]: "F"}},
                        {$unset: metaFieldName + ".d"}
                    ]),
        ErrorCodes.IllegalOperation);

    // Modify the metaField using dot notation: Remove an embedded field of the metaField
    // and a field that is not the metaField.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName + ".c"]: "C"}, [{$unset: [metaFieldName + ".c", "f3"]}]),
        ErrorCodes.InvalidOptions);

    // Modify the metaField using dot notation: Add an embedded field and add a new field.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName + ".c"]: "C"}, [{$set: {[metaFieldName + ".e"]: "E", n: 1}}]),
        ErrorCodes.InvalidOptions);

    // Replace an entire document.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName + ".c"]: "C"},
                    [{$replaceWith: {_id: 4, t: ISODate(), [metaFieldName]: {"z": "Z"}}}]),
        ErrorCodes.InvalidOptions);

    /************************ Tests updating with a replacement document *************************/
    // Replace a document to have no metaField.
    assert.commandFailedWithCode(
        coll.update({[metaFieldName]: {c: "C", d: 2}}, {f2: {e: "E", f: "F"}, f3: 7}),
        ErrorCodes.InvalidOptions);

    // Replace a document with an empty document.
    assert.commandFailedWithCode(coll.update({[metaFieldName]: {c: "C", d: 2}}, {}),
                                 ErrorCodes.InvalidOptions);
});
}());
