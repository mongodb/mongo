// @tags: [
//   requires_getmore,
//   # getMore is not causally consistent if collection is dropped.
//   does_not_support_causal_consistency,
//   # This test relies on query commands returning specific batch-sized responses.
//   assumes_no_implicit_cursor_exhaustion,
// ]

let t = db.jstests_drop3;
let sub = t.sub;

t.drop();
sub.drop();

for (let i = 0; i < 10; i++) {
    t.insert({});
    sub.insert({});
}

let cursor = t.find().batchSize(2);
let subcursor = sub.find().batchSize(2);

cursor.next();
subcursor.next();
assert.eq(cursor.objsLeftInBatch(), 1);
assert.eq(subcursor.objsLeftInBatch(), 1);

t.drop(); // should invalidate cursor, but not subcursor

assert.throws(function () {
    cursor.itcount();
}); // throws "cursor doesn't exist on server" error on getMore
assert.eq(subcursor.itcount(), 9); // one already seen
