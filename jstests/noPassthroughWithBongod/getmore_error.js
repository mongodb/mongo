// ensure errors in getmore are properly reported to users

var t = db.getmore_error;

for (var i = 0; i < 10; i++) {
    t.insert({_id: i});
}

var cursor = t.find().batchSize(2);  // 1 is a special case

// first batch (only one from OP_QUERY)
assert.eq(cursor.next(), {_id: 0});
assert.eq(cursor.next(), {_id: 1});
assert.eq(cursor.objsLeftInBatch(), 0);

// second batch (first from OP_GETMORE)
assert.eq(cursor.next(), {_id: 2});
assert.eq(cursor.next(), {_id: 3});
assert.eq(cursor.objsLeftInBatch(), 0);

/*
// QUERY_MIGRATION disabling this because it's hard to have a failpoint in 2 parallel
// systems
// make the next OP_GETMORE fail
assert.commandWorked(
    db.adminCommand({configureFailPoint: 'getMoreError', mode: {times: 1}})
);

// attempt to get next batch should fail with a failpoint error
var error = assert.throws(function(){cursor.next();});
if (!error.search(/failpoint/))
    assert(false, "got a non-failpoint error: " + error);
*/

// make sure we won't break other tests by breaking getmore for them
assert.eq(t.find().batchSize(2).itcount(), 10);
