/**
 * Ensure that $replaceRoot followed by $match returns the correct results after the optimization
 * that swaps the two stages in SERVER-88220.
 */

import {arrayEq} from "jstests/aggregation/extras/utils.js";

const coll = db.replace_root_match;
coll.drop();

const nestedSubdocXHasSameTypeDiffValDoc = {
    x: 1,
    subDocument: {x: 6.98, y: 1, subDocument: {x: 1}}
};
const nestedSubdocXHasSameTypeSameValDoc = {
    _id: 2,  // We will later assert that this document is returned and we want a predictable '_id'
             // for this one.
    x: 2,
    subDocument: {x: 2, y: 2, subDocument: {x: 2}}
};
const nestedSubdocXHasDiffTypeDoc = {
    x: 3,
    subDocument: {x: "big", y: "small", subDocument: {x: 3}}
};
const nestedSubdocXIsNestedDoc = {
    x: 5,
    subDocument: {x: "small", y: 5, subDocument: {x: {a: 2}}}
};
const subdocAIsObjectWithNumericStrField = {
    subDocument: {a: {"0": 2}}
};
const subdocAIsArrayWithTwo = {
    subDocument: {a: [2, 3]}
};
const subdocAIsArrayWithoutTwo = {
    subDocument: {a: [1, 0]}
};
const subdocAIsArrayWithObject = {
    subDocument: {a: [{b: 2}]}
};
const subdocAIsObject = {
    subDocument: {a: {b: 3}}
};
const subdocAIsAnotherObject = {
    subDocument: {a: {c: 4}}
};
const subdocAIsArrayOfObjectsWithNumericStrFields = {
    subDocument: {a: [{"0": 4}, {"0": 2}]}
};
const subdocAIsArrayOfObjects = {
    subDocument: {a: [{b: 2}, {b: 2}]}
};
const subdocAIsObjectWithEmptyArray = {
    subDocument: {a: {b: []}}
};
const subdocIsDollarDottedPathWithNumericStrField = {
    subDocument: {"$a.0": 2}
};
const subdocIsDottedPath = {
    subDocument: {"a.b": 4}
};
const subdocIsArray = {
    subDocument: [{a: 4}, {a: 5}]
};
const subdocsHaveDiffNames = {
    x: 5,
    subDocumentA: {x: "small", y: 5, subDocumentB: {x: {a: 2}}}
};

const docs = [
    nestedSubdocXHasSameTypeDiffValDoc,
    nestedSubdocXHasSameTypeSameValDoc,
    nestedSubdocXHasDiffTypeDoc,
    nestedSubdocXIsNestedDoc,
    subdocAIsObjectWithNumericStrField,
    subdocAIsArrayWithTwo,
    subdocAIsArrayWithoutTwo,
    subdocAIsArrayWithObject,
    subdocAIsObject,
    subdocAIsAnotherObject,
    subdocAIsArrayOfObjectsWithNumericStrFields,
    subdocAIsArrayOfObjects,
    subdocAIsObjectWithEmptyArray,
    subdocIsDollarDottedPathWithNumericStrField,
    subdocIsDottedPath
];
assert.commandWorked(coll.insert(docs));

const runTest = (pipeline, expected) => {
    const actual = coll.aggregate(pipeline).toArray();
    assert(arrayEq(expected, actual), {expected, actual});
};

// Same as runTest, but checks that the actual value is equal to the value of
// expected["subDocument"].
const runTestWithSubdocExpectedResult = (pipeline, expected) => {
    expected.forEach((el, index) => {
        expected[index] = el["subDocument"];
    });
    const actual = coll.aggregate(pipeline).toArray();
    assert(arrayEq(expected, actual), {expected, actual});
};

const assertFail = (pipeline) => {
    assert.throwsWithCode(() => coll.aggregate(pipeline), [8105800, 40228]);
};

{
    // Simple filter on subField of subDocument - $replaceRoot
    const pipeline = [{$replaceRoot: {newRoot: "$subDocument"}}, {$match: {x: 2}}];
    const expected = [nestedSubdocXHasSameTypeSameValDoc];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // Simple filter on subField of subDocument - $replaceWith
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {x: 6.98}}];
    const expected = [nestedSubdocXHasSameTypeDiffValDoc];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // Composite filter on subField of subDocument - $replaceWith
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {$or: [{x: "big"}, {x: "small"}]}}];
    const expected = [nestedSubdocXHasDiffTypeDoc, nestedSubdocXIsNestedDoc];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // Multiple matches after replaceWith
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {x: "small"}}, {$match: {y: 5}}];
    const expected = [nestedSubdocXIsNestedDoc];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // Multiple replaceWiths before match should fail because <replacementDocument> resolves to a
    // missing document for some documents in the collection.
    const pipeline =
        [{$replaceWith: "$subDocument"}, {$replaceWith: "$subDocument"}, {$match: {"x.a": 2}}];
    assertFail(pipeline);
}
{
    // replaceWith and match refer to predicates with same name.
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {"subDocument.x": 2}}];
    const expected = [nestedSubdocXHasSameTypeSameValDoc];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // replaceWith followed by match with $expr is correctly processed.
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {$expr: {$eq: ["$x", 2]}}}];
    const expected = [nestedSubdocXHasSameTypeSameValDoc];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // replaceWith on $$ROOT is correctly processed.
    const pipeline = [{$replaceWith: "$$ROOT"}, {$match: {x: 2}}];
    const expected = [nestedSubdocXHasSameTypeSameValDoc];
    runTest(pipeline, expected);
}
{
    // First stage matches on documents that contain the field "subDocument.subDocument.x", followed
    // by multiple replaceWiths, followed by match.
    const pipeline = [
        {$match: {"subDocument.subDocument.x": {$exists: true}}},
        {$replaceWith: "$subDocument"},
        {$replaceWith: "$subDocument"},
        {$match: {"x.a": 2}}
    ];
    const expected = [nestedSubdocXIsNestedDoc["subDocument"]["subDocument"]];
    runTest(pipeline, expected);
}

/**
 * Edge cases of MQL semantics
 */

// $match on "a.0" contains implicit array traversal semantics, matching:
// 1) an object with field "a" and subfield "0"
// 2) an object with field "a" that contains an array value, whose value at index 0 is 2
// 3) an object with field "a" that contains an array of objects, containing an object with
// field "0" of value 2
{
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {"a.0": 2}}];
    const expected = [
        subdocAIsObjectWithNumericStrField,
        subdocAIsArrayWithTwo,
        subdocAIsArrayOfObjectsWithNumericStrFields
    ];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // $match with $expr on values greater than "$a.b" will match on objects whose projected value
    // on "$a.b" is greater than 2 in the MQL sort order. Note that documents with field "a" and an
    // array value will match because $expr must return scalar values, and will evaluate {$project:
    // "$a.b"} to an empty array or another array value. The scalar result [ ] is compared to 2.
    // Also, note that a literal field "a.b" will not match.
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {$expr: {$gt: ["$a.b", 2]}}}];
    const expected = [
        subdocAIsArrayWithTwo,
        subdocAIsArrayWithoutTwo,
        subdocAIsArrayWithObject,
        subdocAIsArrayOfObjectsWithNumericStrFields,
        subdocAIsArrayOfObjects,
        subdocAIsObject,
        subdocAIsObjectWithEmptyArray
    ];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // match on $expr with $literal will match on a field that contains '$' in its name.
    const pipeline = [
        {$replaceWith: "$subDocument"},
        {$match: {$expr: {$eq: [{$getField: {$literal: "$a.0"}}, 2]}}}
    ];
    const expected = [subdocIsDollarDottedPathWithNumericStrField];
    runTestWithSubdocExpectedResult(pipeline, expected);
}
{
    // match on $expr with $getField will match on a dotted field path.
    const pipeline =
        [{$replaceWith: "$subDocument"}, {$match: {$expr: {$gt: [{$getField: "a.b"}, 2]}}}];
    const expected = [subdocIsDottedPath];
    runTestWithSubdocExpectedResult(pipeline, expected);
}

assert.commandWorked(coll.insert(subdocIsArray));
{
    // subDocument is not an object.
    const pipeline = [{$replaceWith: "$subDocument"}, {$match: {a: 3}}];
    assertFail(pipeline);
}
{
    // First stage matches on a subDocument that is not an object. Note that {$type: "object"} will
    // sometimes match on documents with array values at "field".
    const pipeline = [
        {$match: {subDocument: {$type: "object"}}},
        {$replaceWith: "$subDocument"},
        {$match: {a: 3}}
    ];
    assertFail(pipeline);
}
{
    // First stage matches on a subDocument that is not an object or array, followed by replaceWith,
    // followed by match.
    const pipeline = [
        {
            $match:
                {$and: [{subDocument: {$type: "object"}}, {subDocument: {$not: {$type: "array"}}}]}
        },
        {$replaceWith: "$subDocument"},
        {$match: {a: 3}}
    ];
    const expected = [subdocAIsArrayWithTwo];
    runTestWithSubdocExpectedResult(pipeline, expected);
}

assert.commandWorked(coll.insert(subdocsHaveDiffNames));
{
    // First stage matches on documents that contain the field "subDocumentA.subDocumentB.x",
    // followed by multiple replaceWith stages, followed by match.
    const pipeline = [
        {$match: {"subDocumentA.subDocumentB.x": {$exists: true}}},
        {$replaceWith: "$subDocumentA"},
        {$replaceWith: "$subDocumentB"},
        {$match: {"x.a": 2}}
    ];
    const expected = [subdocsHaveDiffNames["subDocumentA"]["subDocumentB"]];
    runTest(pipeline, expected);
}
