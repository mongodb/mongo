/**
 * Test the behavior of $project with pipelines that require the whole document. Specifically, we
 * are targeting that rewrites for $project with $getField, or '$$ROOT'/'$$CURRENT' should only
 * occur in certain situations.
 *
 * @tags: [
 *   requires_timeseries,
 *   requires_fcv_72,
 * ]
 */

const timeField = "t";
const metaField = "meta1";
const coll = db.timeseries_project_pushdown;

function runTest({docs, pipeline, expectedResults}) {
    coll.drop();
    assert.commandWorked(db.createCollection(
        coll.getName(), {timeseries: {timeField: timeField, metaField: metaField}}));
    assert.commandWorked(coll.insertMany(docs));
    const results = coll.aggregate(pipeline).toArray();
    assert.sameMembers(expectedResults, results, () => {
        return `Pipeline: ${tojson(pipeline)}. Explain: ${
            tojson(coll.explain().aggregate(pipeline))}`;
    });
}

//
// The following tests confirm the behavior of queries in the form: {$getField: "fieldName"}.
//
(function testMeta_OnlyStringField() {
    runTest({
        docs: [{_id: 1, [timeField]: new Date(), [metaField]: 2}],
        pipeline: [{$project: {new: {$getField: metaField}, _id: 0}}],
        expectedResults: [{new: 2}]
    });
})();

// $getField does not traverse objects, and should not be rewritten when it relies on a mix of
// metaField and measurement fields. Let's validate that behavior.
(function testDottedPath_OnlyStringField() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2, "a.b": 3, a: {b: 2}},
            {_id: 2, [timeField]: new Date(), [metaField]: 2, a: {b: 2}}  // missing field.
        ],
        pipeline: [{$project: {new: {$add: [`$${metaField}`, {$getField: "a.b"}]}}}],
        expectedResults: [{_id: 1, new: 5}, {_id: 2, new: null}]
    });
})();

//
// The following tests confirm the behavior of queries in the form: {$getField: {$literal:
// "fieldName"}}.
//
(function testDottedPath_LiteralExpr() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2, "a.$b": 3, a: {b: 4}},
            {_id: 2, [timeField]: new Date(), [metaField]: 2, a: {b: 2}}  // missing field.
        ],
        pipeline: [{$project: {new: {$add: [`$${metaField}`, {$getField: {$literal: "a.$b"}}]}}}],
        expectedResults: [{_id: 1, new: 5}, {_id: 2, new: null}]
    });
}());

// There is a difference between the metaField "meta1", and "$meta1". Field paths are allowed to
// have a '$' and are accessed by $getField. We need to validate we return the correct results when
// we have both the metaField and "$meta1".
(function testDollarMeta_LiteralExpr() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2},
            {_id: 2, [timeField]: new Date(), [metaField]: 2, "$meta1": 3}  // missing field.
        ],
        pipeline: [{$project: {new: {$add: [`$${metaField}`, {$getField: {$literal: "$meta1"}}]}}}],
        expectedResults: [{_id: 1, new: null}, {_id: 2, new: 5}]
    });
})();

//
// The following tests confirm the behavior of queries in the form: {$getField: <expr>}.
//
(function testMetaField_FieldExpr() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2},
            {_id: 2, [timeField]: new Date(), [metaField]: 4, "$meta1": 3}
        ],
        pipeline: [{
            $project: {
                new: {$add: [`$${metaField}`, {$getField: {$concat: ["m", "e", "t", "a", "1"]}}]}
            }
        }],
        expectedResults: [{_id: 1, new: 4}, {_id: 2, new: 8}]
    });
})();

// When we rely on both the metaField and a measurementField we should not perform the rewrite and
// return the correct result.
(function testDottedPath_FieldExpr() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2},
            {_id: 2, [timeField]: new Date(), [metaField]: 2, "a.b.c": 3}
        ],
        pipeline: [{
            $project:
                {new: {$add: [`$${metaField}`, {$getField: {$cond: [false, null, "a.b.c"]}}]}}
        }],
        expectedResults: [{_id: 1, new: null}, {_id: 2, new: 5}]
    });
})();

//
// The following tests confirm the behavior of queries in the form: {$getField: {input: "string",
// field: "string"}}.
//
(function testMetaField_FieldAndInputString() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: {b: 4}},
            {_id: 2, [timeField]: new Date(), [metaField]: {c: 5}}  // missing subfield.
        ],
        pipeline: [{$project: {new: {$getField: {input: `$${metaField}`, field: "b"}}}}],
        expectedResults: [{_id: 1, new: 4}, {_id: 2}]
    });
})();

// Validate the correct results are returned when there is a field with '$' inside the metaField.
// The rewrite should be valid and return the correct field.
(function testMetaFieldObj_FieldAndInputString() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: {"a.$b": 4}},
            {_id: 2, [timeField]: new Date(), [metaField]: {c: 5}}  // missing subfield.
        ],
        pipeline: [{$project: {new: {$getField: {input: `$${metaField}`, field: "a.$b"}}}}],
        expectedResults: [{_id: 1, new: 4}, {_id: 2}]
    });
})();

// When we rely on both the metaField and a measurementField we should not perform the rewrite and
// return the correct result.
(function testDollarMeta_FieldAndInputString() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2, a: {"$meta1": 4}},
            {_id: 2, [timeField]: new Date(), [metaField]: 2, a: {c: 5}}  // missing subfield.
        ],

        pipeline: [{
            $project: {
                new: {
                    $add: [`$${metaField}`, {$getField: {input: "$a", field: {$literal: "$meta1"}}}]
                }
            }
        }],
        expectedResults: [{_id: 1, new: 6}, {_id: 2, new: null}]
    });
})();

// same test as above but with $addFields and not $project.
(function testDollarMeta_FieldAndInputString_AddFields() {
    const time = new Date();
    runTest({
        docs: [
            {_id: 1, [timeField]: time, [metaField]: 2, a: {"$meta1": 4}},
            {_id: 2, [timeField]: time, [metaField]: 2, a: {c: 5}}  // missing subfield.
        ],
        pipeline: [{
            $addFields: {
                new: {
                    $add: [`$${metaField}`, {$getField: {input: "$a", field: {$literal: "$meta1"}}}]
                }
            }
        }],
        expectedResults: [
            {[timeField]: time, [metaField]: 2, a: {"$meta1": 4}, _id: 1, new: 6},
            {[timeField]: time, [metaField]: 2, a: {c: 5}, _id: 2, new: null}
        ]
    });
})();

//
// The following tests confirm the behavior of queries in the form: {$getField: {input: <expr>,
// field: <expr>}}.
//
(function testMetaField_FieldAndInputExpr() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: {field: 3}},
            {_id: 2, [timeField]: new Date(), [metaField]: {notField: 4}}  // missing subfield.
        ],
        pipeline: [{
            $project: {
                new: {
                    $getField: {
                        input: {$cond: [true, "$meta1", null]},
                        field: {$concat: ["f", "i", "e", "l", "d"]}
                    }
                }
            }
        }],
        expectedResults: [{_id: 1, new: 3}, {_id: 2}]
    });
})();

// When we rely on both the metaField and a measurementField we should not perform the rewrite and
// return the correct result.
(function testDottedPath_FieldAndInputExpr() {
    runTest({
        docs: [
            {_id: 1, [timeField]: new Date(), [metaField]: 2, a: {"b.c": 3}},
            {_id: 2, [timeField]: new Date(), [metaField]: 2, a: {b: {c: 4}}}  // missing field.
        ],
        pipeline: [{
            $project: {
                new: {
                    $add: [
                        `$${metaField}`,
                        {
                            $getField: {
                                input: {$cond: [true, "$a", null]},
                                field: {$concat: ["b", ".", "c"]}
                            }
                        }
                    ]
                }
            }
        }],
        expectedResults: [{_id: 1, new: 5}, {_id: 2, new: null}]
    });
})();

// This test validates that $project with '$$ROOT' which requires the whole document returns the
// correct results.
(function testProject_WithROOT() {
    const time = new Date();
    runTest({
        docs: [
            {_id: 1, [timeField]: time, [metaField]: 2, a: 2},
            {_id: 2, [timeField]: time, [metaField]: 2, b: 3}
        ],
        pipeline: [{$project: {new: "$$ROOT", _id: 1}}],
        expectedResults: [
            {_id: 1, new: {_id: 1, [timeField]: time, [metaField]: 2, a: 2}},
            {_id: 2, new: {_id: 2, [timeField]: time, [metaField]: 2, b: 3}}
        ]
    });
})();

(function testAddFields_WithROOT() {
    const time = new Date();
    runTest({
        docs: [
            {_id: 1, [timeField]: time, [metaField]: 2, a: 2},
            {_id: 2, [timeField]: time, [metaField]: 2, b: 3}
        ],
        pipeline: [{$addFields: {new: "$$ROOT"}}],
        expectedResults: [
            {
                _id: 1,
                [timeField]: time,
                [metaField]: 2,
                a: 2,
                new: {_id: 1, [timeField]: time, [metaField]: 2, a: 2}
            },
            {
                _id: 2,
                [timeField]: time,
                [metaField]: 2,
                b: 3,
                new: {_id: 2, [timeField]: time, [metaField]: 2, b: 3}
            }
        ]
    });
})();
