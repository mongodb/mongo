t = db.jstests_drop3;
sub = t.sub;

t.drop();
sub.drop();

for (var i = 0; i < 10; i++) {
    t.insert({});
    sub.insert({});
}

var cursor = t.find().batchSize(2);
var subcursor = sub.find().batchSize(2);

cursor.next();
subcursor.next();
assert.eq(cursor.objsLeftInBatch(), 1);
assert.eq(subcursor.objsLeftInBatch(), 1);

t.drop();  // should invalidate cursor, but not subcursor

assert.throws(function() {
    cursor.itcount();
});                                 // throws "cursor doesn't exist on server" error on getMore
assert.eq(subcursor.itcount(), 9);  // one already seen
