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
const coll = db.timeseries_project;
let pipeline = [];

function checkResult({expectedResult, pipeline}) {
    assert.docEq(expectedResult,
                 coll.aggregate(pipeline).toArray(),
                 `Result of pipeline ${tojson(pipeline)} which was executed as ${
                     tojson(coll.explain().aggregate(pipeline))}`);
}

// A $project stage that immediately follows unpacking might be incorporated into the unpacking
// stage. Test that the unpacking stage correctly hides the fields that are dropped by the project,
// whether that's a meta, a time or a measurement field.
(function testAccessingDroppedMetaField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    assert.commandWorked(coll.insert({_id: 42, time: ISODate(), meta: 1, a: "a"}));

    checkResult({
        pipeline: [{$project: {_id: 1}}, {$project: {x: "$meta"}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$addFields: {x: "$meta"}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: "$meta"}}}],
        expectedResult: [{}],
    });
})();

(function testAccessingDroppedTimeField() {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: 'time'}}));

    assert.commandWorked(coll.insert({_id: 42, time: ISODate(), a: "a"}));

    checkResult({
        pipeline: [{$project: {_id: 1}}, {$project: {x: "$time"}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$addFields: {x: "$time"}}],
        expectedResult: [{_id: 42}],
    });
    checkResult({
        pipeline: [{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: "$time"}}}],
        expectedResult: [{}],
    });
})();

(function testAccessingDroppedMeasurementField() {
    coll.drop();
    assert.commandWorked(db.createCollection(coll.getName(), {timeseries: {timeField: 'time'}}));

    assert.commandWorked(coll.insert({_id: 42, time: ISODate(), a: "a"}));

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
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    assert.commandWorked(coll.insertMany([
        {_id: 42, time: ISODate(), meta: 1},
        {_id: 43, time: ISODate(), meta: 2},
    ]));

    // With no filters the unpack stage would include no fields.
    checkResult({
        pipeline: [{$replaceRoot: {newRoot: {x: 17}}}, {$addFields: {y: "$time"}}],
        expectedResult: [{x: 17}, {x: 17}],
    });

    // A 'metaField' filter is pushed below unpacking so the unpack stage would include no fields.
    checkResult({
        pipeline:
            [{$match: {meta: 2}}, {$replaceRoot: {newRoot: {x: 17}}}, {$addFields: {y: "$time"}}],
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
            {$addFields: {y: "$time", z: "$_id"}}
        ],
        expectedResult: [{x: 17}],
    });
})();

// Test that when $project redefines a field name, the unpacking stage does not overwrite it.
(function testProjectRedefinesField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    const docDate = ISODate();

    assert.commandWorked(
        coll.insert({_id: 0, time: docDate, meta: 4, a: {b: 1}, b: 3, c: [{}, {}]}));
    let result = [];

    // Check that measurements being unpacked don't overwrite metadata projection pushdown fields.
    result =
        coll.aggregate([{
                $project:
                    {a: 1, b: "$meta", c: {$multiply: [2, "$meta"]}, d: {$multiply: [2, "$meta"]}}
            }])
            .toArray();
    assert.docEq([{_id: 0, a: {b: 1}, b: 4, c: 8, d: 8}], result);

    // Same as above, but keep the rest of the document.
    result = coll.aggregate([{$set: {b: "$meta"}}]).toArray();
    assert.docEq([{_id: 0, time: docDate, meta: 4, a: {b: 1}, b: 4, c: [{}, {}]}], result);

    // Check that nested meta project is not overwritten by the unpacked value.
    result = coll.aggregate([{$project: {"a.b": "$meta"}}]).toArray();
    assert.docEq([{_id: 0, a: {b: 4}}], result);

    // Check that meta project pushed down writes to each value in an array.
    result = coll.aggregate([{$project: {"c.a": "$meta"}}]).toArray();
    assert.docEq([{_id: 0, c: [{a: 4}, {a: 4}]}], result);

    // Replace meta field with unpacked field.
    result = coll.aggregate([{$project: {"meta": "$b"}}]).toArray();
    assert.docEq([{_id: 0, meta: 3}], result);

    // Replace meta field with time field.
    result = coll.aggregate([{$project: {"meta": "$time"}}]).toArray();
    assert.docEq([{_id: 0, meta: docDate}], result);

    // Replace meta field with constant.
    result = coll.aggregate([{$project: {"meta": {$const: 5}}}]).toArray();
    assert.docEq([{_id: 0, meta: 5}], result);

    // Make sure the time field can be overwritten by the meta field correctly.
    result = coll.aggregate([{$set: {time: "$meta"}}]).toArray();
    assert.docEq([{_id: 0, time: 4, meta: 4, a: {b: 1}, b: 3, c: [{}, {}]}], result);

    // Check that the time field can be overwritten by the an unpacked field correctly.
    result = coll.aggregate([{$set: {time: "$b"}}]).toArray();
    assert.docEq([{_id: 0, time: 3, meta: 4, a: {b: 1}, b: 3, c: [{}, {}]}], result);

    // Make sure the time field can be overwritten by a constant correctly.
    result = coll.aggregate([{$project: {time: {$const: 5}}}]).toArray();
    assert.docEq([{_id: 0, time: 5}], result);

    // Test that a pushed down meta field projection can correctly be excluded.
    result = coll.aggregate([{$set: {b: "$meta"}}, {$unset: "a"}]).toArray();
    assert.docEq([{_id: 0, time: docDate, meta: 4, b: 4, c: [{}, {}]}], result);

    // Exclude behavior for time field.
    result = coll.aggregate([{$set: {b: "$time"}}, {$unset: "a"}]).toArray();
    assert.docEq([{_id: 0, time: docDate, meta: 4, b: docDate, c: [{}, {}]}], result);

    // Exclude behavior for consecutive projects.
    result = coll.aggregate([{$set: {b: "$meta"}}, {$unset: "meta"}]).toArray();
    assert.docEq([{_id: 0, time: docDate, a: {b: 1}, b: 4, c: [{}, {}]}], result);

    // Test that an exclude does not overwrite meta field pushdown.
    result = coll.aggregate([{$unset: "b"}, {$set: {b: "$meta"}}]).toArray();
    assert.docEq([{_id: 0, time: docDate, meta: 4, a: {b: 1}, b: 4, c: [{}, {}]}], result);
})();

// Test that a field reference in a $project stage refers to the stage's input document rather than
// the newly defined field with the same name.
(function testProjectRedefinesAndUsesField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    const timestamp = new Date("2019-10-11T14:39:18.670Z");
    const doc = {
        time: timestamp,
        meta: "meta",
        a: "a",
        b: "b",
        obj: {a: "obj.a"},
    };
    assert.commandWorked(coll.insert(doc));
    let result = [];

    // Redefining and using the top-level 'metaField' in the same stage should use the field from
    // the upstream (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, meta: "$b", x: "$meta"}}]).toArray();
    assert.docEq([{meta: "b", x: "meta"}], result);

    // Redefining and using the 'timeField' in the same stage should use the field from the upstream
    // (unpack) stage.
    result = coll.aggregate([{$project: {_id: 0, time: "$b", x: "$time"}}]).toArray();
    assert.docEq([{time: "b", x: timestamp}], result);

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
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    const timestamp = new Date("2019-10-11T14:39:18.670Z");
    const doc = {
        _id: 42,
        time: timestamp,
        meta: "meta",
        a: "a",
    };
    assert.commandWorked(coll.insert(doc));
    let result = {};

    // Redefining and using the top-level 'metaField' in the same stage should use the field from
    // the upstream (unpack) stage.
    result = coll.aggregate([{$addFields: {meta: "$a", x: "$meta"}}]).toArray();
    assert.docEq([{_id: 42, time: timestamp, meta: "a", a: "a", x: "meta"}], result);

    // Redefining and using the 'timeField' in the same stage should use the field from the upstream
    // (unpack) stage.
    result = coll.aggregate([{$addFields: {time: "$a", x: "$time"}}]).toArray();
    assert.docEq([{_id: 42, time: "a", meta: "meta", a: "a", x: timestamp}], result);

    // Redefining and using a top-level measurement field in the same stage should use the field
    // from the upstream (unpack) stage.
    result = coll.aggregate([{$addFields: {a: "$_id", x: "$a"}}]).toArray();
    assert.docEq([{_id: 42, time: timestamp, meta: "meta", a: 42, x: "a"}], result);

    // Redefining and using a measurement field in an expression in the same stage should use the
    // field from the upstream (unpack) stage.
    result = coll.aggregate([{$addFields: {a: "$_id", x: {$toUpper: "$a"}}}]).toArray();
    assert.docEq([{_id: 42, time: timestamp, meta: "meta", a: 42, x: "A"}], result);
})();

// Test that an includes projection that doesn't include previously included computed meta fields
// outputs correctly.
(function testIncludeComputedMetaFieldThenProjectWithoutComputedMetaField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'tag'}}));

    const doc = {_id: 0, time: ISODate("2024-01-01T00:00:00.000Z"), tag: {field: "A"}};
    assert.commandWorked(coll.insert(doc));
    let result = {};

    // Added field which uses meta should not be in the result.
    result =
        coll.aggregate(
                [{$addFields: {time: {$dateFromParts: {year: "$tag.none"}}}}, {$project: {tag: 1}}])
            .toArray();
    assert.docEq([{"tag": {"field": "A"}, "_id": 0}], result);

    // Test with non-null value.
    result = coll.aggregate([{$addFields: {time: {$toLower: "$tag.field"}}}, {$project: {tag: 1}}])
                 .toArray();
    assert.docEq([{"tag": {"field": "A"}, "_id": 0}], result);

    // Testing with $match between $addFields and $project.
    result = coll.aggregate([
                     {$addFields: {time: {$dateFromParts: {year: "$tag.none"}}}},
                     {$match: {tag: {field: "A"}}},
                     {$project: {tag: 1}}
                 ])
                 .toArray();
    assert.docEq([{"tag": {field: "A"}, "_id": 0}], result);

    // Test with non-null value.
    result =
        coll.aggregate(
                [{$addFields: {time: "$tag.field"}}, {$match: {time: "A"}}, {$project: {tag: 1}}])
            .toArray();
    assert.docEq([{"tag": {field: "A"}, "_id": 0}], result);

    // Testing with $project after $project.
    result = coll.aggregate([{$project: {tag: 1}}, {$project: {time: 1}}]).toArray();
    assert.docEq([{"_id": 0}], result);

    // Testing with $set.
    result = coll.aggregate([{$set: {tag: "$tag.field"}}, {$project: {time: 1}}]).toArray();
    assert.docEq([{"_id": 0, "time": ISODate("2024-01-01T00:00:00Z")}], result);

    // Test that computedMetaFields that are included by the $project are still included.
    result = coll.aggregate([
                     {$addFields: {hi: {$dateFromParts: {year: "$tag.none"}}}},
                     {$addFields: {bye: {$dateFromParts: {year: "$tag.none"}}}},
                     {$project: {hi: 1}}
                 ])
                 .toArray();
    assert.docEq([{"_id": 0, "hi": null}], result);

    // Test with non-null value.
    result = coll.aggregate([
                     {$addFields: {hi: "$tag.field"}},
                     {$addFields: {bye: "$tag.field"}},
                     {$project: {hi: 1}}
                 ])
                 .toArray();
    assert.docEq([{"_id": 0, "hi": "A"}], result);

    // Test that $project that replaces field that is used works.
    result = coll.aggregate([
                     {$addFields: {hello: "$tag.field"}},
                     {$project: {newTime: "$time", time: "$tag.field"}}
                 ])
                 .toArray();

    assert.docEq([{"_id": 0, newTime: ISODate("2024-01-01T00:00:00.000Z"), time: "A"}], result);

    // Test adding fields and including one of them.
    result = coll.aggregate([
                     {$addFields: {a: {$toLower: "$tag.field"}}},
                     {$addFields: {b: {$toUpper: "$tag.field"}}},
                     {$project: {b: 1}}
                 ])
                 .toArray();
    assert.docEq([{b: "A", "_id": 0}], result);
})();

// Test that an excludes projection that excludes previously included computed meta fields
// outputs correctly.
(function testIncludeComputedMetaFieldThenExcludeComputedMetaField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'tag'}}));

    const doc = {_id: 0, time: ISODate("2024-01-01T00:00:00.000Z"), tag: {field: "A"}};
    assert.commandWorked(coll.insert(doc));
    let result = {};

    // Excluded '_computedMetaProjField' should not be included.
    result = coll.aggregate([
                     {$addFields: {hello: {$dateFromParts: {year: "$tag.none"}}}},
                     {$project: {hello: 0}}
                 ])
                 .toArray();
    assert.docEq([{"time": ISODate("2024-01-01T00:00:00Z"), "tag": {"field": "A"}, "_id": 0}],
                 result);

    // Test with non-null value.
    result = coll.aggregate([{$addFields: {time: {$toLower: "$tag.field"}}}, {$project: {time: 0}}])
                 .toArray();
    assert.docEq([{"tag": {"field": "A"}, "_id": 0}], result);

    // Testing with $match between $addFields and $project.
    result = coll.aggregate([
                     {$addFields: {time: {$dateFromParts: {year: "$tag.none"}}}},
                     {$match: {tag: {field: "A"}}},
                     {$project: {time: 0}}
                 ])
                 .toArray();
    assert.docEq([{"tag": {field: "A"}, "_id": 0}], result);

    // Test with non-null value.
    result =
        coll.aggregate(
                [{$addFields: {time: "$tag.field"}}, {$match: {time: "A"}}, {$project: {time: 0}}])
            .toArray();
    assert.docEq([{"tag": {field: "A"}, "_id": 0}], result);

    // Testing with exclude $project after include $project.
    result = coll.aggregate([{$project: {tag: 1}}, {$project: {tag: 0}}]).toArray();
    assert.docEq([{"_id": 0}], result);

    // Testing with $set.
    result = coll.aggregate([{$set: {tag: "$tag.field"}}, {$project: {tag: 0}}]).toArray();
    assert.docEq([{"time": ISODate("2024-01-01T00:00:00Z"), "_id": 0}], result);

    // Exclude one added field.
    result = coll.aggregate([
                     {$addFields: {hi: {$dateFromParts: {year: "$tag.none"}}}},
                     {$addFields: {bye: {$dateFromParts: {year: "$tag.none"}}}},
                     {$project: {hi: 0}}
                 ])
                 .toArray();
    assert.docEq(
        [{"time": ISODate("2024-01-01T00:00:00Z"), "tag": {"field": "A"}, "_id": 0, "bye": null}],
        result);

    // Test with non-null value.
    result = coll.aggregate([
                     {$addFields: {hi: "$tag.field"}},
                     {$addFields: {bye: "$tag.field"}},
                     {$project: {hi: 0}}
                 ])
                 .toArray();
    assert.docEq(
        [{"time": ISODate("2024-01-01T00:00:00Z"), "tag": {"field": "A"}, "_id": 0, "bye": "A"}],
        result);

    // Test adding fields and excluding one of them.
    result = coll.aggregate([
                     {$addFields: {a: {$toLower: "$tag.field"}}},
                     {$addFields: {b: {$toUpper: "$tag.field"}}},
                     {$project: {b: 0}}
                 ])
                 .toArray();
    assert.docEq(
        [{"time": ISODate("2024-01-01T00:00:00Z"), "tag": {"field": "A"}, "_id": 0, "a": "a"}],
        result);
})();
