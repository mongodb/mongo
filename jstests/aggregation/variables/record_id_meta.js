/**
 * Tests that requests with $meta: "recordId" correctly return the recordId.
 * @tags: [
 *  requires_fcv_83
 * ]
 */
const testDB = db.getSiblingDB("record_id_meta");
const collName = jsTestName();
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.getCollection(collName);
assert.commandWorked(coll.insert({a: 1}));

/**
 * Test that when $meta: "recordId" is requested it is added to the results
 */
const addFieldsResponse = coll.aggregate([{$addFields: {rid: {$meta: "recordId"}}}]);
assert.eq(Object.keys(addFieldsResponse._batch[0]), ["_id", "a", "rid"]);
assert.eq(addFieldsResponse._batch[0].rid, NumberLong(1));
const projectResponse = coll.aggregate([{$project: {"a": 1, rid: {$meta: "recordId"}}}]);
assert.eq(Object.keys(projectResponse._batch[0]), ["_id", "a", "rid"]);
assert.eq(projectResponse._batch[0].rid, NumberLong(1));

/**
 * Test that when $meta: "recordId" is not requested it is not added to the results
 */
const addFieldsNoRidResponse = coll.aggregate([{$addFields: {}}]);
assert.eq(Object.keys(addFieldsNoRidResponse._batch[0]), ["_id", "a"]);
const projectNoRidResponse = coll.aggregate([{$project: {"a": 1}}]);
assert.eq(Object.keys(projectNoRidResponse._batch[0]), ["_id", "a"]);
