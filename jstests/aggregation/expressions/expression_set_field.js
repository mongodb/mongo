/**
 * Tests basic functionality of the $setField expression and the $unsetField alias.
 * @tags: [assumes_unsharded_collection]
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For assertArrayEq.

const coll = db.expression_set_field;
coll.drop();

function buildTestCase(i) {
    return {
        _id: i,
        x: i,
        y: "c",
        "a$b": "foo",
        "a.b": "bar",
        "a.$b": 5,
        ".xy": i,
        ".$xz": i,
        "..zz": i,
        "$a": 10,
        "$x.$y": 20,
        "$x..$y": {"$a": 1, "$b..$c": 2},
        c: {d: "x"},
        e: {"$f": 30}
    };
}

function setTestCaseField(i, field, value) {
    var res = buildTestCase(i);
    res[field] = value;
    return {_id: i, test: res};
}

function unsetTestCaseField(i, field) {
    var res = buildTestCase(i);
    delete res[field];
    return {_id: i, test: res};
}

function getTestCasesForSet(field, value) {
    return [setTestCaseField(0, field, value), setTestCaseField(1, field, value)];
}

function getTestCasesForUnset(field, value) {
    return [unsetTestCaseField(0, field, value), unsetTestCaseField(1, field, value)];
}

// Test that $setField fails with the provided 'code' for invalid arguments 'setFieldArgs'.
function assertSetFieldFailedWithCode(setFieldArgs, code) {
    const error =
        assert.throws(() => coll.aggregate([{$project: {test: {$setField: setFieldArgs}}}]));
    assert.commandFailedWithCode(error, code);
}

// Test that $unsetField fails with the provided 'code' for invalid arguments 'unsetFieldArgs'.
function assertUnsetFieldFailedWithCode(unsetFieldArgs, code) {
    const error =
        assert.throws(() => coll.aggregate([{$project: {test: {$unsetField: unsetFieldArgs}}}]));
    assert.commandFailedWithCode(error, code);
}

// Test that $setField returns the 'expected' results for the given arguments 'setFieldArgs'.
function assertSetFieldResultsEq(setFieldArgs, expected) {
    assertPipelineResultsEq([{$project: {_id: 1, test: {$setField: setFieldArgs}}}], expected);
}

// Test that $unsetField returns the 'expected' results for the given arguments 'unsetFieldArgs'.
function assertUnsetFieldResultsEq(unsetFieldArgs, expected) {
    assertPipelineResultsEq([{$project: {_id: 1, test: {$unsetField: unsetFieldArgs}}}], expected);
}

// Test that $setField correctly sets the 'field' to 'value' in all documents in 'coll'.
function assertSetFieldInRootDoc(field, value) {
    // Wrap 'field' argument to $setField in $const to allow test cases containing dots and dollars.
    assertPipelineResultsEq(
        [{$project: {_id: 1, test: {$setField: {field: {$const: field}, input: "$$ROOT", value}}}}],
        getTestCasesForSet(field, value));
}

// Test that $setField and $unsetField both correctly unset the 'field' in all documents in 'coll'.
function assertUnsetFieldInRootDoc(field) {
    // Wrap 'field' argument to $setField in $const to allow test cases containing dots and dollars.
    assertPipelineResultsEq(
        [{
            $project: {
                _id: 1,
                test: {$setField: {field: {$const: field}, input: "$$ROOT", value: "$$REMOVE"}}
            }
        }],
        getTestCasesForUnset(field));

    // Test $unsetField alias as well.
    assertPipelineResultsEq(
        [{$project: {_id: 1, test: {$unsetField: {field: {$const: field}, input: "$$ROOT"}}}}],
        getTestCasesForUnset(field));
}

// Test the given 'pipeline' returns the 'expected' results.
function assertPipelineResultsEq(pipeline, expected) {
    const actual = coll.aggregate(pipeline).toArray();
    assertArrayEq({actual, expected});
}

const isDotsAndDollarsEnabled = db.adminCommand({getParameter: 1, featureFlagDotsAndDollars: 1})
                                    .featureFlagDotsAndDollars.value;

if (!isDotsAndDollarsEnabled) {
    // Verify that $setField is not available if the feature flag is set to false and don't
    // run the rest of the test.
    assertSetFieldFailedWithCode({field: "a", input: {a: "b"}, value: "foo"}, 31325);
    return;
}

for (let i = 0; i < 2; i++) {
    assert.commandWorked(coll.insert(buildTestCase(i)));
}

// Test that $setField fails with a document missing named arguments.

// Field and value are missing.
assertSetFieldFailedWithCode({input: {a: "b"}}, 4161102);
assertSetFieldFailedWithCode({}, 4161102);
assertUnsetFieldFailedWithCode({input: {a: "b"}}, 4161102);
assertUnsetFieldFailedWithCode({}, 4161102);

// Field is missing.
assertSetFieldFailedWithCode({value: "a"}, 4161102);
assertSetFieldFailedWithCode({value: "a", input: {a: "b"}}, 4161102);

// Value is missing in $setField.
assertSetFieldFailedWithCode({field: "a"}, 4161103);
assertSetFieldFailedWithCode({field: "a", input: {a: "b"}}, 4161103);

// Value is present in $unsetField.
assertUnsetFieldFailedWithCode({field: "a", value: "foo"}, 4161101);
assertUnsetFieldFailedWithCode({field: "a", value: null, input: {a: "b"}}, 4161101);

// Input is missing.
assertSetFieldFailedWithCode({field: null, value: 0}, 4161109);
assertSetFieldFailedWithCode({field: "foo", value: 0}, 4161109);
assertUnsetFieldFailedWithCode({field: null}, 4161109);
assertUnsetFieldFailedWithCode({field: "foo"}, 4161109);

// Test that $setField fails with one or more arguments of incorrect type.
assertSetFieldFailedWithCode({field: true, input: {a: "b"}, value: 24}, 4161107);
assertSetFieldFailedWithCode({field: {"a": 1}, input: {"a": 1}, value: 24}, 4161106);
assertSetFieldFailedWithCode({field: 33, input: 33, value: 24}, 4161107);
assertSetFieldFailedWithCode({field: "a", input: true, value: 24}, 4161105);

// Test that $unsetField fails with one or more arguments of incorrect type.
assertUnsetFieldFailedWithCode({field: true, input: {a: "b"}}, 4161107);
assertUnsetFieldFailedWithCode({field: {"a": 1}, input: {"a": 1}}, 4161106);
assertUnsetFieldFailedWithCode({field: 33, input: 33}, 4161107);
assertUnsetFieldFailedWithCode({field: "a", input: true}, 4161105);

// Test that $setField fails when 'field' is not a constant string argument.
assertSetFieldFailedWithCode({field: null, input: {}, value: 0}, 4161107);
assertSetFieldFailedWithCode({field: "$field_path", input: {}, value: 0}, 4161108);
assertSetFieldFailedWithCode(
    {field: {$concat: ["a.b", ".", "c"]}, input: {$const: {"a.b.c": 5}}, value: 12345}, 4161106);

// Test that $unsetField fails when 'field' is not a constant string argument.
assertUnsetFieldFailedWithCode({field: null, input: {}}, 4161107);
assertUnsetFieldFailedWithCode({field: "$field_path", input: {}}, 4161108);
assertUnsetFieldFailedWithCode({field: {$concat: ["a.b", ".", "c"]}, input: {$const: {"a.b.c": 5}}},
                               4161106);

// $setField does not accept an argument that is not an object.
assertSetFieldFailedWithCode(5, 4161100);
assertSetFieldFailedWithCode(true, 4161100);
assertSetFieldFailedWithCode({$add: [2, 3]}, 4161101);
assertSetFieldFailedWithCode("foo", 4161100);

// $unsetField does not accept an argument that is not an object.
assertUnsetFieldFailedWithCode(5, 4161100);
assertUnsetFieldFailedWithCode(true, 4161100);
assertUnsetFieldFailedWithCode({$add: [2, 3]}, 4161101);
assertUnsetFieldFailedWithCode("foo", 4161100);

// Test that $setField fails with a document with invalid arguments.
assertSetFieldFailedWithCode({field: "a", input: {a: "b"}, unknown: true}, 4161101);
assertSetFieldFailedWithCode({field: "a", input: {a: "b"}, value: 24, unknown: true}, 4161101);

// Test that $setField correctly sets the field to the given value in the provided object.
assertSetFieldResultsEq({field: "a", input: {a: "b"}, value: 24},
                        [{_id: 0, test: {a: 24}}, {_id: 1, test: {a: 24}}]);
assertSetFieldResultsEq({field: "a", input: {b: "b"}, value: 24},
                        [{_id: 0, test: {b: "b", a: 24}}, {_id: 1, test: {b: "b", a: 24}}]);
assertSetFieldResultsEq({field: "a", input: {}, value: 24},
                        [{_id: 0, test: {a: 24}}, {_id: 1, test: {a: 24}}]);

// Test that $setField correctly removes the field in the provided object.
assertSetFieldResultsEq({field: "a", input: {a: "b"}, value: "$$REMOVE"},
                        [{_id: 0, test: {}}, {_id: 1, test: {}}]);
assertSetFieldResultsEq({field: "a", input: {b: "b"}, value: "$$REMOVE"},
                        [{_id: 0, test: {b: "b"}}, {_id: 1, test: {b: "b"}}]);
assertSetFieldResultsEq({field: "a", input: {}, value: "$$REMOVE"},
                        [{_id: 0, test: {}}, {_id: 1, test: {}}]);

// Test that $unsetField correctly removes the field in the provided object.
assertUnsetFieldResultsEq({field: "a", input: {a: "b"}}, [{_id: 0, test: {}}, {_id: 1, test: {}}]);
assertUnsetFieldResultsEq({field: "a", input: {b: "b"}},
                          [{_id: 0, test: {b: "b"}}, {_id: 1, test: {b: "b"}}]);
assertUnsetFieldResultsEq({field: "a", input: {}}, [{_id: 0, test: {}}, {_id: 1, test: {}}]);

// Test that $setField returns null when given a nullish 'input'.
assertSetFieldResultsEq({field: "not_going_to_be_a_field", input: null, value: 0},
                        [{_id: 0, test: null}, {_id: 1, test: null}]);

// Test that $unsetField returns null when given a nullish 'input'.
assertUnsetFieldResultsEq({field: "not_going_to_be_a_field", input: null},
                          [{_id: 0, test: null}, {_id: 1, test: null}]);

// Test that $setField correctly updates 'field' in the $$ROOT object to 'value', or clears it if
// value is set to $$REMOVE.
const testFields = ["a", "a$b", "a.b", "x", "a.$b", ".xy", ".$xz", "..zz", "$a", "$x.$y", "$x..$y"];
for (const field in testFields) {
    assertSetFieldInRootDoc(field, null);
    assertSetFieldInRootDoc(field, 12345);
    assertSetFieldInRootDoc(field, "foo");
    assertSetFieldInRootDoc(field, {a: 23, xy: {a: 1, b: 2}});
    assertUnsetFieldInRootDoc(field);
}

// Test that $setField treats dotted fields as key literals instead of field paths. Note that it is
// necessary to use $const in places, otherwise object field validation would reject some of these
// field names.
assertSetFieldResultsEq({field: "a.b", input: {$const: {"a.b": "b"}}, value: 12345},
                        [{_id: 0, test: {"a.b": 12345}}, {_id: 1, test: {"a.b": 12345}}]);

assertSetFieldResultsEq({field: ".ab", input: {$const: {".ab": "b"}}, value: 12345},
                        [{_id: 0, test: {".ab": 12345}}, {_id: 1, test: {".ab": 12345}}]);

assertSetFieldResultsEq({field: "ab.", input: {$const: {"ab.": "b"}}, value: 12345},
                        [{_id: 0, test: {"ab.": 12345}}, {_id: 1, test: {"ab.": 12345}}]);

assertSetFieldResultsEq({field: "a.b.c", input: {$const: {"a.b.c": 5}}, value: 12345},
                        [{_id: 0, test: {"a.b.c": 12345}}, {_id: 1, test: {"a.b.c": 12345}}]);

assertSetFieldResultsEq({field: "a.b.c", input: {a: {b: {c: 5}}}, value: 12345}, [
    {_id: 0, test: {"a.b.c": 12345, a: {b: {c: 5}}}},
    {_id: 1, test: {"a.b.c": 12345, a: {b: {c: 5}}}}
]);

// Test that $setField works with fields that contain '$'.
assertSetFieldResultsEq({field: "a$b", input: {"a$b": "b"}, value: 12345},
                        [{_id: 0, test: {"a$b": 12345}}, {_id: 1, test: {"a$b": 12345}}]);

assertSetFieldResultsEq({field: "a$b.b", input: {$const: {"a$b.b": 5}}, value: 12345},
                        [{_id: 0, test: {"a$b.b": 12345}}, {_id: 1, test: {"a$b.b": 12345}}]);

assertSetFieldResultsEq({field: {$const: "a$b.b"}, input: {$const: {"a$b.b": 5}}, value: 12345},
                        [{_id: 0, test: {"a$b.b": 12345}}, {_id: 1, test: {"a$b.b": 12345}}]);

assertSetFieldResultsEq({field: {$const: "$b.b"}, input: {$const: {"$b.b": 5}}, value: 12345},
                        [{_id: 0, test: {"$b.b": 12345}}, {_id: 1, test: {"$b.b": 12345}}]);

assertSetFieldResultsEq({field: {$const: "$b"}, input: {$const: {"$b": 5}}, value: 12345},
                        [{_id: 0, test: {"$b": 12345}}, {_id: 1, test: {"$b": 12345}}]);

assertSetFieldResultsEq({field: {$const: "$.ab"}, input: {$const: {"$.ab": 5}}, value: 12345},
                        [{_id: 0, test: {"$.ab": 12345}}, {_id: 1, test: {"$.ab": 12345}}]);

assertSetFieldResultsEq({field: {$const: "$$xz"}, input: {$const: {"$$xz": 5}}, value: 12345},
                        [{_id: 0, test: {"$$xz": 12345}}, {_id: 1, test: {"$$xz": 12345}}]);

// Test cases where $setField stages are nested.
assertSetFieldResultsEq(
    {
        field: "b.c",
        input: {$setField: {field: "b.c", input: {$const: {"b.c": {a: 5}}}, value: "replace me"}},
        value: "something"
    },
    [{_id: 0, test: {"b.c": "something"}}, {_id: 1, test: {"b.c": "something"}}]);

assertSetFieldResultsEq({
    field: "x",
    input: {$setField: {field: "b.c", input: {$const: {"b.c": {a: 5}}}, value: "forget-me-not"}},
    value: "something"
},
                        [
                            {_id: 0, test: {"b.c": "forget-me-not", x: "something"}},
                            {_id: 1, test: {"b.c": "forget-me-not", x: "something"}}
                        ]);

assertSetFieldResultsEq({
    field: "a",
    input: {$setField: {field: "b.d", input: {$const: {"b.c": {a: 5}}}, value: "forget-me-not"}},
    value: "$_id"
},
                        [
                            {_id: 0, test: {"b.c": {a: 5}, "b.d": "forget-me-not", "a": 0}},
                            {_id: 1, test: {"b.c": {a: 5}, "b.d": "forget-me-not", "a": 1}}
                        ]);
assertSetFieldResultsEq({
    field: {$const: "$h.$i"},
    input: {$setField: {field: {$const: "$g.."}, input: "$e", value: "forget-me-not"}},
    value: "$_id"
},
                        [
                            {_id: 0, test: {"$f": 30, "$g..": "forget-me-not", "$h.$i": 0}},
                            {_id: 1, test: {"$f": 30, "$g..": "forget-me-not", "$h.$i": 1}}
                        ]);

// Test $getField and $setField together.
assertPipelineResultsEq([{
                            $project: {
                                _id: 1,
                                result: {
                                    $eq: [
                                        {
                                            $getField: {
                                                field: "foo",
                                                input: {
                                                    $setField: {
                                                        field: "foo",
                                                        input: "$$ROOT",
                                                        value: 1234,
                                                    }
                                                }
                                            }
                                        },
                                        1234
                                    ]
                                }
                            }
                        }],
                        [{_id: 0, result: true}, {_id: 1, result: true}]);
assertSetFieldResultsEq({
    field: {$const: "$x..$y"},
    input: {},
    value: {
        $setField:
            {field: {$const: "$a"}, input: {$getField: {$const: "$x..$y"}}, value: "forget-me-not"}
    },
},
                        [
                            {_id: 0, test: {"$x..$y": {"$a": "forget-me-not", "$b..$c": 2}}},
                            {_id: 1, test: {"$x..$y": {"$a": "forget-me-not", "$b..$c": 2}}}
                        ]);

// Test $setField and $-prefixed fields with $replaceWith.
assert(coll.drop());
assert.commandWorked(coll.insert({_id: 0, "x": 1, "a$b": "foo", "$x..$y": {"$a": 1, "$b..$c": 2}}));
assertPipelineResultsEq(
    [{
        $replaceWith: {
            $setField: {
                field: {$const: "$x..$y"},
                input: "$$ROOT",
                value: {
                    $setField: {
                        field: {$const: "$a"},
                        input: {$getField: {$const: "$x..$y"}},
                        value: "forget-me-not"
                    }
                },
            }
        }
    }],
    [{_id: 0, "x": 1, "a$b": "foo", "$x..$y": {"$a": "forget-me-not", "$b..$c": 2}}]);
})();
