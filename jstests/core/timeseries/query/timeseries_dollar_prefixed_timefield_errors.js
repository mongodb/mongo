/**
 * This test verifies that timeseries collections with a dollar-prefixed time field will fail
 * upon creation (see BF-40520).
 *
 * @tags: [
 *   # createCollection may fail with transient stepdown errors before reaching validation, so we
 *   # cannot reliably assert BadValue.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_83,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

// Bad timeField.
assert.commandFailedWithCode(db.createCollection(coll.getName(), {timeseries: {timeField: "$t"}}), ErrorCodes.BadValue);

// Good timeField, bad metaField.
assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {timeseries: {timeField: "t", metaField: "$m"}}),
    ErrorCodes.BadValue,
);

// Bad timeField, good metaField.
assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {timeseries: {timeField: "$t", metaField: "m"}}),
    ErrorCodes.BadValue,
);

// Bad timeField and metaField.
assert.commandFailedWithCode(
    db.createCollection(coll.getName(), {timeseries: {timeField: "$t", metaField: "$m"}}),
    ErrorCodes.BadValue,
);

// Seed a regular collection as the $out input.
const inputColl = db[jsTestName() + "_input"];
inputColl.drop();
assert.commandWorked(inputColl.insert({t: ISODate(), m: 0}));

const outCollName = jsTestName() + "_out";

// Bad timeField via $out timeseries options.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: inputColl.getName(),
        pipeline: [
            {
                $out: {
                    db: db.getName(),
                    coll: outCollName,
                    timeseries: {timeField: "$t"},
                },
            },
        ],
        cursor: {},
    }),
    ErrorCodes.BadValue,
);

// Bad metaField via $out timeseries options.
assert.commandFailedWithCode(
    db.runCommand({
        aggregate: inputColl.getName(),
        pipeline: [
            {
                $out: {
                    db: db.getName(),
                    coll: outCollName,
                    timeseries: {timeField: "t", metaField: "$m"},
                },
            },
        ],
        cursor: {},
    }),
    ErrorCodes.BadValue,
);
