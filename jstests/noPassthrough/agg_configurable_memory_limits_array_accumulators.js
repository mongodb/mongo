/**
 * Tests that $concatArrays and $setUnion accumulators respect configurable memory limits.
 * @tags: [requires_fcv_81]
 */

const conn = MongoRunner.runMongod();
assert.neq(null, conn, "mongod was unable to start up");
const db = conn.getDB("test");
const coll = db.agg_configurable_memory_limit;

// The approximate size of the strings below is 22-25 bytes.
const stringSize = 22;
const nSets = 20;
// For the $concatArrays accumulator, 5 strings will be added to the accumulated array for each set
// of strings inserted to the collection. When this memory limit is set, $concatArrays should fail
// due to exceeding the memory limit because it's about half of what we expect to need.
const memLimitArray = nSets * 5 * stringSize / 2;
// For the $setUnion accumulator, 1 new unique string will be added to the accumulated array for
// each set of strings inserted to the collection (nSets). Additionally, "string 0", "string 1",
// "string 2", and "string 3" will be added with each of the nSets, but only appear once in the
// output (+4). When this memory limit is set, $setUnion should fail due to exceeding the memory
// limit because it's about half of what we expect to need.
const memLimitSet = (nSets + 4) * stringSize / 2;

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < nSets; i++) {
    bulk.insert({_id: 5 * i + 0, y: ["string 0"]});
    bulk.insert({_id: 5 * i + 1, y: ["string 1"]});
    bulk.insert({_id: 5 * i + 2, y: ["string 2"]});
    bulk.insert({_id: 5 * i + 3, y: ["string 3"]});
    bulk.insert({_id: 5 * i + 4, y: ["string " + 5 * i + 4]});
}
assert.commandWorked(bulk.execute());

(function testInternalQueryMaxConcatArraysBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(
        () => coll.aggregate([{$group: {_id: null, strings: {$concatArrays: "$y"}}}]));

    // Now lower the limit to test that its configuration is obeyed.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxConcatArraysBytes: memLimitArray}));
    assert.throwsWithCode(
        () => coll.aggregate([{$group: {_id: null, strings: {$concatArrays: "$y"}}}]),
        ErrorCodes.ExceededMemoryLimit);
}());

(function testInternalQueryMaxSetUnionBytesSetting() {
    // Test that the default 100MB memory limit isn't reached with our data.
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$setUnion: "$y"}}}]));

    // Test that $setUnion needs a tighter limit than $concatArrays (because some of the strings are
    // the same).
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxSetUnionBytes: memLimitArray}));
    assert.doesNotThrow(() => coll.aggregate([{$group: {_id: null, strings: {$setUnion: "$y"}}}]));

    // Test that a tighter limit for $setUnion is obeyed.
    assert.commandWorked(
        db.adminCommand({setParameter: 1, internalQueryMaxSetUnionBytes: memLimitSet}));
    assert.throwsWithCode(() => coll.aggregate([{$group: {_id: null, strings: {$setUnion: "$y"}}}]),
                          ErrorCodes.ExceededMemoryLimit);
}());

MongoRunner.stopMongod(conn);
