/**
 * Test the behavior of $project on time-series collections.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   requires_fcv_62,
 * ]
 */
const coll = db.timeseries_project;

// A $project stage that immediately follows unpacking might be incorporated into the unpacking
// stage. Test that the unpacking stage correctly hides the fields that are dropped by the project.
(function testAccessingDroppedField() {
    coll.drop();
    assert.commandWorked(
        db.createCollection(coll.getName(), {timeseries: {timeField: 'time', metaField: 'meta'}}));

    const timestamp = ISODate();

    assert.commandWorked(coll.insert({_id: 42, time: timestamp, meta: 1, a: "a"}));

    // Access dropped 'metaField'.
    assert.docEq([{_id: 42}],
                 coll.aggregate([{$project: {_id: 1}}, {$project: {x: "$meta"}}]).toArray());
    assert.docEq([{_id: 42}],
                 coll.aggregate([{$project: {_id: 1}}, {$addFields: {x: "$meta"}}]).toArray());
    assert.docEq(
        [{}],
        coll.aggregate([{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: "$meta"}}}]).toArray());

    // Access dropped 'timeField'.
    assert.docEq([{_id: 42}],
                 coll.aggregate([{$project: {_id: 1}}, {$project: {x: "$time"}}]).toArray());
    assert.docEq([{_id: 42}],
                 coll.aggregate([{$project: {_id: 1}}, {$addFields: {x: "$time"}}]).toArray());
    assert.docEq(
        [{}],
        coll.aggregate([{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: "$time"}}}]).toArray());

    // Access dropped measurement.
    assert.docEq([{_id: 42}],
                 coll.aggregate([{$project: {_id: 1}}, {$project: {x: "$a"}}]).toArray());
    assert.docEq([{_id: 42}],
                 coll.aggregate([{$project: {_id: 1}}, {$addFields: {x: "$a"}}]).toArray());
    assert.docEq(
        [{}],
        coll.aggregate([{$project: {_id: 1}}, {$replaceRoot: {newRoot: {x: "$a"}}}]).toArray());
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
