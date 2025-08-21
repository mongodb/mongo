/**
 * Test the $createObjectId expression.
 * @tags: [
 *  featureFlagMqlJsEngineGap,
 *  requires_fcv_83
 * ]
 */
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";

const collName = jsTestName();
const coll = db[collName];
const docCount = 100;

function setupCollection() {
    assertDropCollection(db, collName);
    const bulk = coll.initializeUnorderedBulkOp();
    for (let i = 0; i < docCount; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());
}

// Validate the $createObjectId expression works and returns a different
// value each time it is executed.
{
    setupCollection();
    const resultArray = coll.aggregate([{$project: {_id: {$createObjectId: {}}}}]).toArray();

    // All generated values are ObjectIds.
    for (let result of resultArray) {
        assert.eq("object", typeof result["_id"]);
        assert(result["_id"].toString().startsWith("ObjectId("));
    }
    // All generated ObjectIds are unique.
    assert.eq(new Set(resultArray.map((res) => res["_id"].toString())).size, resultArray.length);
}

// Test with invalid arguments.
setupCollection();
const failedToParseCode = 9;
{
    // non-empty object
    const error = assert.throws(() =>
        coll.aggregate([{$project: {_id: {$createObjectId: {"key": "value"}}}}]).toArray(),
    );
    assert.commandFailedWithCode(error, failedToParseCode);
}
{
    // null
    const error = assert.throws(() => coll.aggregate([{$project: {_id: {$createObjectId: null}}}]).toArray());
    assert.commandFailedWithCode(error, failedToParseCode);
}
{
    // array
    const error = assert.throws(() => coll.aggregate([{$project: {_id: {$createObjectId: ["argument"]}}}]).toArray());
    assert.commandFailedWithCode(error, failedToParseCode);
}
{
    // object id
    const error = assert.throws(() => coll.aggregate([{$project: {_id: {$createObjectId: ObjectId()}}}]).toArray());
    assert.commandFailedWithCode(error, failedToParseCode);
}
{
    // string
    const error = assert.throws(() => coll.aggregate([{$project: {_id: {$createObjectId: "argument"}}}]).toArray());
    assert.commandFailedWithCode(error, failedToParseCode);
}
