// @tags: [
//   requires_non_retryable_writes,
//   # Time series collections do not support indexing array values in measurement fields.
//   exclude_from_timeseries_crud_passthrough,
// ]

// Check that the positional operator works properly when an index only match is used for the update
// query spec.  SERVER-5067

// TODO (SERVER-117130): Remove the mongos pinning once the related issue is resolved.
// When a database is dropped, a stale router will report "database not found" error for
// deletes (instead of "ok") when pauseMigrationsDuringMultiUpdates is enabled.
if (TestData.pauseMigrationsDuringMultiUpdates) {
    TestData.pinToSingleMongos = true;
}

const t = db[jsTestName()];
t.drop();

function testPositionalInc() {
    assert.commandWorked(t.remove({}));
    assert.commandWorked(t.save({a: [{b: "match", count: 0}]}));
    assert.commandWorked(t.update({"a.b": "match"}, {$inc: {"a.$.count": 1}}));
    // Check that the positional $inc succeeded.
    assert(t.findOne({"a.count": 1}));
}

testPositionalInc();

// Now check with a non multikey index.
assert.commandWorked(t.createIndex({"a.b": 1}));
testPositionalInc();
