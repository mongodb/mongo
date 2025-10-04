/**
 * Tests the behavior of an $merge stage which encounters an error in the middle of processing. We
 * don't guarantee any particular behavior in this scenario, but this test exists to make sure
 * nothing horrendous happens and to characterize the current behavior.
 * @tags: [
 *   # TODO SERVER-79448: Investigate why the test timeouts on TSAN variant.
 *   tsan_incompatible,
 * ]
 */
import {dropWithoutImplicitRecreate, withEachMergeMode} from "jstests/aggregation/extras/merge_helpers.js";
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db.batch_writes;
const outColl = db.batch_writes_out;
coll.drop();
outColl.drop();

// Test with 2 very large documents that do not fit into a single batch.
const kSize15MB = 15 * 1024 * 1024;
const largeArray = "a".repeat(kSize15MB);
assert.commandWorked(coll.insert({_id: 0, a: largeArray}));
assert.commandWorked(coll.insert({_id: 1, a: largeArray}));

// Make sure the $merge succeeds without any duplicate keys.
withEachMergeMode(({whenMatchedMode, whenNotMatchedMode}) => {
    // Skip the combination of merge modes which will fail depending on the contents of the
    // source and target collection, as this will cause the aggregation to fail.
    if (whenMatchedMode == "fail" || whenNotMatchedMode == "fail") return;

    coll.aggregate([
        {
            $merge: {
                into: outColl.getName(),
                whenMatched: whenMatchedMode,
                whenNotMatched: whenNotMatchedMode,
            },
        },
    ]);
    assert.eq(whenNotMatchedMode == "discard" ? 0 : 2, outColl.find().itcount());
    outColl.drop();
});

coll.drop();
for (let i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({_id: i, a: i}));
}

// Create a unique index on 'a' in the output collection to create a unique key violation when
// running the $merge. The second document to be written ({_id: 1, a: 1}) will conflict with the
// existing document in the output collection. We use a unique index on a field other than _id
// because whenMatched: "replace" will not change _id when one already exists.
dropWithoutImplicitRecreate(outColl.getName());
assert.commandWorked(outColl.insert({_id: 2, a: 1}));
assert.commandWorked(outColl.createIndex({a: 1}, {unique: true}));

// Test that the writes for $merge are unordered, meaning the operation continues even if it
// encounters a duplicate key error. We don't guarantee any particular behavior in this case,
// but this test is meant to characterize the current behavior.
assertErrorCode(
    coll,
    [{$merge: {into: outColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}],
    ErrorCodes.DuplicateKey,
);
assert.soon(() => {
    return outColl.find().itcount() == 9;
});

// Note that a $sort comes before the $merge to guarantee that {_id: 1, a: 1} will always be
// seen before {_id: 2, a: 2}. If {_id: 1, a: 1} is seen first, it gets inserted and will
// trigger a DuplicateKeyError since a's value is duplicated by {_id: 2, a: 1}. If {_id: 2, a:
// 2} is seen first, then it will make the replacement {_id: 2, a: 1} => {_id: 2, a: 2},
// preventing a duplicate key error from arising later on.
assertErrorCode(
    coll,
    [{$sort: {a: 1}}, {$merge: {into: outColl.getName(), whenMatched: "replace", whenNotMatched: "insert"}}],
    ErrorCodes.DuplicateKey,
);
assert.soon(() => {
    return outColl.find().itcount() == 9;
});
