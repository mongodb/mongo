/**
 * Tests that empty objects as expressions in $set and $addFields aggregation stages are permitted.
 *
 * @tags: [
 *   requires_fcv_61
 * ]
 */

(function() {
"use strict";

// For arrayEq.
load("jstests/aggregation/extras/utils.js");

const collName = jsTest.name();
const coll = db.getCollection(collName);
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

// Test that empty objects as expressions are permitted, with and without the $literal wrapper.
initObj["otherField"] = {};
assertAddFieldsResult({"otherField": {}}, initObj);
assertAddFieldsResult({"otherField": {$literal: {}}}, initObj);

// Test that nested empty objects are permitted.
initObj["otherField"]["b"] = {};
assertAddFieldsResult({"otherField": {"b": {}}}, initObj);

// Test that empty literal definitions are permitted.
initObj["otherField"] = [];
assertAddFieldsResult({"otherField": []}, initObj);

initObj["otherField"] = "value";  // Reset the input object.

// Test that a new empty field is permitted.
initObj["newField"] = {};
assertAddFieldsResult({"newField": {}}, initObj);
})();
