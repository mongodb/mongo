/**
 * Test the behavior of $project on time-series collections.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_62,
 *   # Aggregation with explain may return incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   # "Explain of a resolved view must be executed by mongos"
 *   directly_against_shardsvrs_incompatible,
 * ]
 */
const coll = db[jsTestName()];
let pipeline = [];

const timeFieldName = "time";
const metaFieldName = "m";
const metaFieldValue = "metaValue";

function checkResult({expectedResult, pipeline}) {
    assert.docEq(
        expectedResult,
        coll.aggregate(pipeline).toArray(),
        `Result of pipeline ${tojson(pipeline)} which was executed as ${tojson(coll.explain().aggregate(pipeline))}`,
    );
}

// A $project stage that immediately follows unpacking might be incorporated into the unpacking
// stage. Test that the unpacking stage correctly hides the fields that are dropped by the project,
// whether that's a meta, a time or a measurement field.
(function testAccessingDroppedMetaField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    assert.commandWorked(coll.insert({_id: 42, [timeFieldName]: ISODate(), [metaFieldName]: 1, a: "a"}));

    checkResult({
        pipeline: [{$project: {_id: 1}}, {$project: {x: `$${metaFieldName}`}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$addFields: {x: `$${metaFieldName}`}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: `$${metaFieldName}`}}}],
        expectedResult: [{}],
    });
})();

(function testAccessingDroppedTimeField() {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    assert.commandWorked(coll.insert({_id: 42, [timeFieldName]: ISODate(), a: "a"}));

    checkResult({
        pipeline: [{$project: {_id: 1}}, {$project: {x: `$${timeFieldName}`}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$addFields: {x: `$${timeFieldName}`}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: `$${timeFieldName}`}}}],
        expectedResult: [{}],
    });
})();

(function testAccessingDroppedMeasurementField() {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

    assert.commandWorked(coll.insert({_id: 42, [timeFieldName]: ISODate(), a: "a"}));

    checkResult({
        pipeline: [{$project: {_id: 1}}, {$project: {x: "$a"}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$addFields: {x: "$a"}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: "$a"}}}],
        expectedResult: [{}],
    });
})();

// The $replaceRoot stage itself isn't incorporated into unpacking but it causes the unpack
// stage to include no fields, as if everything was projected away. It should still unpack the
// correct _number_ of events. In SBE we rely on the 'timeField' to do this, but the field
// shouldn't be visible to the rest of the query.
(function testEmptyIncludeSet() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    assert.commandWorked(
        coll.insertMany([
            {_id: 42, [timeFieldName]: ISODate(), [metaFieldName]: 1},
            {_id: 43, [timeFieldName]: ISODate(), [metaFieldName]: 2},
        ]),
    );

    // With no filters the unpack stage would include no fields.
    checkResult({
        pipeline: [{$replaceRoot: {newRoot: {x: 17}}}, {$addFields: {y: `$${timeFieldName}`}}],
        expectedResult: [{x: 17}, {x: 17}],
    });

    // A 'metaField' filter is pushed below unpacking so the unpack stage would include no fields.
    checkResult({
        pipeline: [
            {$match: {[metaFieldName]: 2}},
            {$replaceRoot: {newRoot: {x: 17}}},
            {$addFields: {y: `$${timeFieldName}`}},
        ],
        expectedResult: [{x: 17}],
    });

    // With a trivially "false" event filter the unpack stage would still include no fields.
    checkResult({
        pipeline: [{$match: {_id: {$in: []}}}, {$replaceRoot: {newRoot: {x: 17}}}],
        expectedResult: [],
    });

    // A non-trivial event filter would cause the unpack stage to include the field for the filter.
    checkResult({
        pipeline: [
            {$match: {_id: 42}},
            {$replaceRoot: {newRoot: {x: 17}}},
            {$addFields: {y: `$${timeFieldName}`, z: "$_id"}},
        ],
        expectedResult: [{x: 17}],
    });
})();

// Test that when $project redefines a field name, the unpacking stage does not overwrite it.
(function testProjectRedefinesField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    const docDate = ISODate();

    assert.commandWorked(
        coll.insert({_id: 0, [timeFieldName]: docDate, [metaFieldName]: 4, a: {b: 1}, b: 3, c: [{}, {}]}),
    );
    let result = [];

    // Check that measurements being unpacked don't overwrite metadata projection pushdown fields.
    result = coll
        .aggregate([
            {
                $project: {
                    a: 1,
                    b: `$${metaFieldName}`,
                    c: {$multiply: [2, `$${metaFieldName}`]},
                    d: {$multiply: [2, `$${metaFieldName}`]},
                },
            },
        ])
        .toArray();
    assert.docEq([{_id: 0, a: {b: 1}, b: 4, c: 8, d: 8}], result);

    // Same as above, but keep the rest of the document.
    result = coll.aggregate([{$set: {b: `$${metaFieldName}`}}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: docDate, [metaFieldName]: 4, a: {b: 1}, b: 4, c: [{}, {}]}], result);

    // Check that nested meta project is not overwritten by the unpacked value.
    result = coll.aggregate([{$project: {"a.b": `$${metaFieldName}`}}]).toArray();
    assert.docEq([{_id: 0, a: {b: 4}}], result);

    // Check that meta project pushed down writes to each value in an array.
    result = coll.aggregate([{$project: {"c.a": `$${metaFieldName}`}}]).toArray();
    assert.docEq([{_id: 0, c: [{a: 4}, {a: 4}]}], result);

    // Replace meta field with unpacked field.
    result = coll.aggregate([{$project: {[metaFieldName]: "$b"}}]).toArray();
    assert.docEq([{_id: 0, [metaFieldName]: 3}], result);

    // Replace meta field with time field.
    result = coll.aggregate([{$project: {[metaFieldName]: `$${timeFieldName}`}}]).toArray();
    assert.docEq([{_id: 0, [metaFieldName]: docDate}], result);

    // Replace meta field with constant.
    result = coll.aggregate([{$project: {[metaFieldName]: {$const: 5}}}]).toArray();
    assert.docEq([{_id: 0, [metaFieldName]: 5}], result);

    // Make sure the time field can be overwritten by the meta field correctly.
    result = coll.aggregate([{$set: {[timeFieldName]: `$${metaFieldName}`}}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: 4, [metaFieldName]: 4, a: {b: 1}, b: 3, c: [{}, {}]}], result);

    // Check that the time field can be overwritten by the an unpacked field correctly.
    result = coll.aggregate([{$set: {[timeFieldName]: "$b"}}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: 3, [metaFieldName]: 4, a: {b: 1}, b: 3, c: [{}, {}]}], result);

    // Make sure the time field can be overwritten by a constant correctly.
    result = coll.aggregate([{$project: {[timeFieldName]: {$const: 5}}}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: 5}], result);

    // Test that a pushed down meta field projection can correctly be excluded.
    result = coll.aggregate([{$set: {b: `$${metaFieldName}`}}, {$unset: "a"}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: docDate, [metaFieldName]: 4, b: 4, c: [{}, {}]}], result);

    // Exclude behavior for time field.
    result = coll.aggregate([{$set: {b: `$${timeFieldName}`}}, {$unset: "a"}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: docDate, [metaFieldName]: 4, b: docDate, c: [{}, {}]}], result);

    // Exclude behavior for consecutive projects.
    result = coll.aggregate([{$set: {b: `$${metaFieldName}`}}, {$unset: metaFieldName}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: docDate, a: {b: 1}, b: 4, c: [{}, {}]}], result);

    // Test that an exclude does not overwrite meta field pushdown.
    result = coll.aggregate([{$unset: "b"}, {$set: {b: `$${metaFieldName}`}}]).toArray();
    assert.docEq([{_id: 0, [timeFieldName]: docDate, [metaFieldName]: 4, a: {b: 1}, b: 4, c: [{}, {}]}], result);
})();

// Test that a field reference in a $project stage refers to the stage's input document rather than
// the newly defined field with the same name.
(function testProjectRedefinesAndUsesField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    const timestamp = new Date("2019-10-11T14:39:18.670Z");
    const doc = {
        [timeFieldName]: timestamp,
        [metaFieldName]: metaFieldValue,
        a: "a",
        b: "b",
        obj: {a: "obj.a"},
    };
    assert.commandWorked(coll.insert(doc));
    let result = [];

    // Redefining and using the top-level 'metaField' in the same stage should use the field from
    // the upstream (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, [metaFieldName]: "$b", x: `$${metaFieldName}`}}]).toArray();
    assert.docEq([{[metaFieldName]: "b", x: metaFieldValue}], result);

    // Redefining and using the 'timeField' in the same stage should use the field from the upstream
    // (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, [timeFieldName]: "$b", x: `$${timeFieldName}`}}]).toArray();
    assert.docEq([{[timeFieldName]: "b", x: timestamp}], result);

    // Redefining and using a top-level measurement field in the same stage should use the field
    // from the upstream (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, a: "$b", x: "$a"}}]).toArray();
    assert.docEq([{a: "b", x: "a"}], result);

    // Redefining and using a sub-measurement field in the same stage should use the field from the
    // upstream (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, obj: "$b", x: "$obj.a"}}]).toArray();
    assert.docEq([{obj: "b", x: "obj.a"}], result);

    // Redefining and using a measurement field in an expression in the same stage should use the
    // field from the upstream (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, a: "$b", x: {$toUpper: "$a"}}}]).toArray();
    assert.docEq([{a: "b", x: "A"}], result);
})();

// Test that a field reference in a $addFields stage refers to the stage's input document rather
// than the newly defined field with the same name.
(function testAddFieldsRedefinesAndUsesField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    const timestamp = new Date("2019-10-11T14:39:18.670Z");
    const doc = {
        _id: 42,
        [timeFieldName]: timestamp,
        [metaFieldName]: metaFieldValue,
        a: "a",
    };
    assert.commandWorked(coll.insert(doc));
    let result = {};

    // Redefining and using the top-level 'metaField' in the same stage should use the field from
    // the upstream (unpack) stage.
    result = coll.aggregate([{$addFields: {[metaFieldName]: "$a", x: `$${metaFieldName}`}}]).toArray();
    assert.docEq([{_id: 42, [timeFieldName]: timestamp, [metaFieldName]: "a", a: "a", x: metaFieldValue}], result);

    // Redefining and using the 'timeField' in the same stage should use the field from the upstream
    // (unpack) stage.
    result = coll.aggregate([{$addFields: {[timeFieldName]: "$a", x: `$${timeFieldName}`}}]).toArray();
    assert.docEq([{_id: 42, [timeFieldName]: "a", [metaFieldName]: metaFieldValue, a: "a", x: timestamp}], result);

    // Redefining and using a top-level measurement field in the same stage should use the field
    // from the upstream (unpack) stage.
    result = coll.aggregate([{$addFields: {a: "$_id", x: "$a"}}]).toArray();
    assert.docEq([{_id: 42, [timeFieldName]: timestamp, [metaFieldName]: metaFieldValue, a: 42, x: "a"}], result);

    // Redefining and using a measurement field in an expression in the same stage should use the
    // field from the upstream (unpack) stage.
    result = coll.aggregate([{$addFields: {a: "$_id", x: {$toUpper: "$a"}}}]).toArray();
    assert.docEq([{_id: 42, [timeFieldName]: timestamp, [metaFieldName]: metaFieldValue, a: 42, x: "A"}], result);
})();

// Test that an includes projection that doesn't include previously included computed meta fields
// outputs correctly.
(function testIncludeComputedMetaFieldThenProjectWithoutComputedMetaField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    const doc = {_id: 0, [timeFieldName]: ISODate("2024-01-01T00:00:00.000Z"), [metaFieldName]: {field: "A"}};
    assert.commandWorked(coll.insert(doc));
    let result = {};

    // Added field which uses meta should not be in the result.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$project: {[metaFieldName]: 1}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {"field": "A"}, "_id": 0}], result);

    // Test with non-null value.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: {$toLower: `$${metaFieldName}.field`}}},
            {$project: {[metaFieldName]: 1}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {"field": "A"}, "_id": 0}], result);

    // Testing with $match between $addFields and $project.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$match: {[metaFieldName]: {field: "A"}}},
            {$project: {[metaFieldName]: 1}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {field: "A"}, "_id": 0}], result);

    // Test with non-null value.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: `$${metaFieldName}.field`}},
            {$match: {[timeFieldName]: "A"}},
            {$project: {[metaFieldName]: 1}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {field: "A"}, "_id": 0}], result);

    // Testing with $project after $project.
    result = coll.aggregate([{$project: {[metaFieldName]: 1}}, {$project: {[timeFieldName]: 1}}]).toArray();
    assert.docEq([{"_id": 0}], result);

    // Testing with $set.
    result = coll
        .aggregate([{$set: {[metaFieldName]: `$${metaFieldName}.field`}}, {$project: {[timeFieldName]: 1}}])
        .toArray();
    assert.docEq([{"_id": 0, [timeFieldName]: ISODate("2024-01-01T00:00:00Z")}], result);

    // Test that computedMetaFields that are included by the $project are still included.
    result = coll
        .aggregate([
            {$addFields: {hi: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$addFields: {bye: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$project: {hi: 1}},
        ])
        .toArray();
    assert.docEq([{"_id": 0, "hi": null}], result);

    // Test with non-null value.
    result = coll
        .aggregate([
            {$addFields: {hi: `$${metaFieldName}.field`}},
            {$addFields: {bye: `$${metaFieldName}.field`}},
            {$project: {hi: 1}},
        ])
        .toArray();
    assert.docEq([{"_id": 0, "hi": "A"}], result);

    // Test that $project that replaces field that is used works.
    result = coll
        .aggregate([
            {$addFields: {hello: `$${metaFieldName}.field`}},
            {$project: {newTime: `$${timeFieldName}`, time: `$${metaFieldName}.field`}},
        ])
        .toArray();

    assert.docEq([{"_id": 0, newTime: ISODate("2024-01-01T00:00:00.000Z"), [timeFieldName]: "A"}], result);

    // Test adding fields and including one of them.
    result = coll
        .aggregate([
            {$addFields: {a: {$toLower: `$${metaFieldName}.field`}}},
            {$addFields: {b: {$toUpper: `$${metaFieldName}.field`}}},
            {$project: {b: 1}},
        ])
        .toArray();
    assert.docEq([{b: "A", "_id": 0}], result);
})();

// Test that an excludes projection that excludes previously included computed meta fields
// outputs correctly.
(function testIncludeComputedMetaFieldThenExcludeComputedMetaField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
    );

    const doc = {_id: 0, [timeFieldName]: ISODate("2024-01-01T00:00:00.000Z"), [metaFieldName]: {field: "A"}};
    assert.commandWorked(coll.insert(doc));
    let result = {};

    // Excluded '_computedMetaProjField' should not be included.
    result = coll
        .aggregate([{$addFields: {hello: {$dateFromParts: {year: `$${metaFieldName}.none`}}}}, {$project: {hello: 0}}])
        .toArray();
    assert.docEq(
        [{"_id": 0, [timeFieldName]: ISODate("2024-01-01T00:00:00Z"), [metaFieldName]: {"field": "A"}}],
        result,
    );

    // Test with non-null value.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: {$toLower: `$${metaFieldName}.field`}}},
            {$project: {[timeFieldName]: 0}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {"field": "A"}, "_id": 0}], result);

    // Testing with $match between $addFields and $project.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$match: {[metaFieldName]: {field: "A"}}},
            {$project: {[timeFieldName]: 0}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {field: "A"}, "_id": 0}], result);

    // Test with non-null value.
    result = coll
        .aggregate([
            {$addFields: {[timeFieldName]: `$${metaFieldName}.field`}},
            {$match: {[timeFieldName]: "A"}},
            {$project: {[timeFieldName]: 0}},
        ])
        .toArray();
    assert.docEq([{[metaFieldName]: {field: "A"}, "_id": 0}], result);

    // Testing with exclude $project after include $project.
    result = coll.aggregate([{$project: {[metaFieldName]: 1}}, {$project: {[metaFieldName]: 0}}]).toArray();
    assert.docEq([{"_id": 0}], result);

    // Testing with $set.
    result = coll
        .aggregate([{$set: {[metaFieldName]: `$${metaFieldName}.field`}}, {$project: {[metaFieldName]: 0}}])
        .toArray();
    assert.docEq([{[timeFieldName]: ISODate("2024-01-01T00:00:00Z"), "_id": 0}], result);

    // Exclude one added field.
    result = coll
        .aggregate([
            {$addFields: {hi: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$addFields: {bye: {$dateFromParts: {year: `$${metaFieldName}.none`}}}},
            {$project: {hi: 0}},
        ])
        .toArray();
    assert.docEq(
        [{[timeFieldName]: ISODate("2024-01-01T00:00:00Z"), [metaFieldName]: {"field": "A"}, "_id": 0, "bye": null}],
        result,
    );

    // Test with non-null value.
    result = coll
        .aggregate([
            {$addFields: {hi: `$${metaFieldName}.field`}},
            {$addFields: {bye: `$${metaFieldName}.field`}},
            {$project: {hi: 0}},
        ])
        .toArray();
    assert.docEq(
        [{[timeFieldName]: ISODate("2024-01-01T00:00:00Z"), [metaFieldName]: {"field": "A"}, "_id": 0, "bye": "A"}],
        result,
    );

    // Test adding fields and excluding one of them.
    result = coll
        .aggregate([
            {$addFields: {a: {$toLower: `$${metaFieldName}.field`}}},
            {$addFields: {b: {$toUpper: `$${metaFieldName}.field`}}},
            {$project: {b: 0}},
        ])
        .toArray();
    assert.docEq(
        [{[timeFieldName]: ISODate("2024-01-01T00:00:00Z"), [metaFieldName]: {"field": "A"}, "_id": 0, "a": "a"}],
        result,
    );
})();
