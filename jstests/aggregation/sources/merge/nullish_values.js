/**
 * Test $merge on null and missing values.
 * @tags: [
 *    requires_fcv_81,
 *    assumes_unsharded_collection
 * ]
 */

const inputColl = db.merge_nullish_input_coll;
const outputColl = db.merge_nullish_output_coll;

for (let mergeMode of ["merge", "replace", "keepExisting"]) {
    let mergePipeline =
        [{$merge: {into: outputColl.getName(), on: "mergeOn", whenMatched: mergeMode}}];
    let expectedDocument = {_id: 0, order: "first"};

    jsTestLog("Testing pipeline: " + tojsononeline(mergePipeline));

    inputColl.drop();
    outputColl.drop();
    outputColl.createIndex({mergeOn: 1}, {unique: true});

    assert.commandWorked(inputColl.insertOne({_id: 0, order: "first"}));
    assert.doesNotThrow(() => inputColl.aggregate(mergePipeline));
    assert.eq(outputColl.findOne({mergeOn: null}, {mergeOn: 0}), expectedDocument);

    assert.commandWorked(inputColl.updateOne({_id: 0}, {$set: {mergeOn: null, order: "second"}}));
    assert.doesNotThrow(() => inputColl.aggregate(mergePipeline));

    if (mergeMode !== "keepExisting") {
        expectedDocument.order = "second";
    }
    assert.eq(sortDoc(outputColl.findOne({mergeOn: null}, {mergeOn: 0})), expectedDocument);
}

inputColl.drop();
outputColl.drop();
outputColl.createIndex({"merge.on": 1}, {unique: true});

// All the following documents should be merged into one with a missing/null "merge.on" field
assert.commandWorked(inputColl.insertMany(
    [{merge: {on: null}, a: 1}, {merge: {}, b: 2}, {c: 3}, {merge: "scalar", d: 4}]));
assert.doesNotThrow(
    () => inputColl.aggregate(
        [{$project: {_id: 0}}, {$merge: {into: outputColl.getName(), on: "merge.on"}}]));
assert.eq(sortDoc(outputColl.findOne({}, {_id: 0, merge: 0})), {a: 1, b: 2, c: 3, d: 4});
