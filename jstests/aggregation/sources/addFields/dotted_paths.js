/**
 * Test the behavior of $addFields in the presence of a dotted field path.
 */
(function() {
const coll = db.add_fields_dotted_paths;
coll.drop();

let initObj = {
    _id: 1,
    arrayField: [1, {subField: [2, {}]}, [1]],
    objField: {p: {q: 1}, subArr: [1]},
    otherField: "value"
};
assert.commandWorked(coll.insert(initObj));

function assertAddFieldsResult(projection, expectedResults) {
    assert.eq(coll.aggregate([{$addFields: projection}]).toArray(), [expectedResults]);
}

// Test that the value gets overwritten when a field exists at a given path.
initObj["objField"]["subArr"] = "newValue";
assertAddFieldsResult({"objField.subArr": "newValue"}, initObj);
assertAddFieldsResult({"objField.subArr": {$literal: "newValue"}}, initObj);

// Test that a new sub-object is created when a field does not exist at a given path. All the
// existing sibling fields are retained.
initObj["objField"] = {
    p: {q: 1},
    subArr: [1],  // Existing fields are retained.
    newSubPath: {b: "newValue"}
};
assertAddFieldsResult({"objField.newSubPath.b": {$literal: "newValue"}}, initObj);
assertAddFieldsResult({"objField.newSubPath.b": "newValue"}, initObj);

// When the value is a nested object.
const valueWithNestedObject = {
    newSubObj: [{p: "newValue"}]
};
initObj["objField"]["newSubPath"] = {
    b: valueWithNestedObject
};
assertAddFieldsResult({"objField.newSubPath.b": valueWithNestedObject}, initObj);
assertAddFieldsResult({"objField.newSubPath.b": {$literal: valueWithNestedObject}}, initObj);
initObj["objField"] = {
    p: {q: 1},
    subArr: [1]
};  // Reset input object.

// When the top level field doesn"t exist, a new nested object is created based on the given path.
initObj["newField"] = {
    newSubPath: {b: "newValue"}
};
assertAddFieldsResult({"newField.newSubPath.b": {$literal: "newValue"}}, initObj);
assertAddFieldsResult({"newField.newSubPath.b": "newValue"}, initObj);

// When the top level field doesn"t exist, a new nested object is created based on the given path
// and the structure of the object in the value.
initObj["newField"]["newSubPath"] = {
    b: valueWithNestedObject
};
assertAddFieldsResult({"newField.newSubPath.b": valueWithNestedObject}, initObj);
assertAddFieldsResult({"newField.newSubPath.b": {$literal: valueWithNestedObject}}, initObj);
delete initObj["newField"];  // Reset.

// Test when the path encounters an array and the value is a scalar.
initObj["arrayField"] = {
    newSubPath: {b: "newValue"}
};
let expectedSubObj = {newSubPath: {b: "newValue"}};
initObj["arrayField"] =
    [expectedSubObj, Object.assign({subField: [2, {}]}, expectedSubObj), [expectedSubObj]];
assertAddFieldsResult({"arrayField.newSubPath.b": {$literal: "newValue"}}, initObj);
assertAddFieldsResult({"arrayField.newSubPath.b": "newValue"}, initObj);

// Test when the path encounters an array and the value is a nested object.
expectedSubObj = {
    newSubPath: {b: valueWithNestedObject}
};
initObj["arrayField"] =
    [expectedSubObj, Object.assign({subField: [2, {}]}, expectedSubObj), [expectedSubObj]];
assertAddFieldsResult({"arrayField.newSubPath.b": valueWithNestedObject}, initObj);
assertAddFieldsResult({"arrayField.newSubPath.b": {$literal: valueWithNestedObject}}, initObj);

// Test when the path encounters multiple arrays and the value is a nested object.
expectedSubObj = {
    subField: {b: valueWithNestedObject}
};
initObj["arrayField"] = [
    expectedSubObj,
    {
        subField:
            [{b: valueWithNestedObject}, {b: valueWithNestedObject}]  // Sub-array is also exploded.
    },
    [expectedSubObj]
];
assertAddFieldsResult({"arrayField.subField.b": valueWithNestedObject}, initObj);
assertAddFieldsResult({"arrayField.subField.b": {$literal: valueWithNestedObject}}, initObj);
})();
