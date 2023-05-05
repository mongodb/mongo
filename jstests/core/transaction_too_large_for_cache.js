/**
 * Tests that an operation requiring more cache than available fails instead of retrying infinitely.
 *
 * @tags: [
 *   does_not_support_config_fuzzer,
 *   requires_fcv_63,
 *   requires_persistence,
 *   requires_non_retryable_writes,
 *   requires_wiredtiger,
 *   no_selinux
 * ]
 */

(function() {
load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.
load("jstests/libs/storage_engine_utils.js");

// TODO (SERVER-39362): remove once parallel suite respects tags properly.
if (!storageEngineIsWiredTiger()) {
    jsTestLog("Skipping test because storage engine is not WiredTiger.");
    return;
}

const doc = {
    x: []
};
for (var j = 0; j < 334000; j++) {
    doc.x.push("" + Math.random() + Math.random());
}

const coll = db[jsTestName()];
coll.drop();

// Maximum amount of indexes is 64. _id is implicit, and sharded collections also have an index on
// the shard key.
assert.commandWorked(coll.createIndex({x: "text"}));
for (let i = 0; i < 61; i++) {
    assert.commandWorked(coll.createIndex({x: 1, ["field" + i]: 1}));
}

// Retry the operation until we eventually hit the TransactionTooLargeForCache. Retry on
// WriteConflict or TemporarilyUnavailable errors, as those are expected to be returned if the
// threshold for TransactionTooLargeForCache is not reached, possibly due to concurrent operations.
assert.soon(() => {
    let result;
    try {
        result = coll.insert(doc);
        assert.commandFailedWithCode(result, ErrorCodes.TransactionTooLargeForCache);
        return true;
    } catch (e) {
        assert.commandFailedWithCode(result,
                                     [ErrorCodes.WriteConflict, ErrorCodes.TemporarilyUnavailable]);
        return false;
    }
}, "Expected operation to eventually fail with TransactionTooLargeForCache error.");
}());
