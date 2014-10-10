// Test queries that set the OplogReplay flag.

var t = db.jstests_query_oplogreplay;
t.drop();

for (var i = 0; i < 100; i++) {
    t.save({_id: i, ts: i});
}

// Missing 'ts' field.
assert.throws(function() {
    t.find().addOption(DBQuery.Option.oplogReplay).next();
});
assert.throws(function() {
    t.find({_id: 3}).addOption(DBQuery.Option.oplogReplay).next();
});

// 'ts' field is not top-level.
assert.throws(function() {
    t.find({$or: [{ts: {$gt: 3}}, {foo: 3}]})
          .addOption(DBQuery.Option.oplogReplay).next();
});
assert.throws(function() {
    t.find({$nor: [{ts: {$gt: 4}}, {foo: 4}]})
          .addOption(DBQuery.Option.oplogReplay).next();
});

// Predicate over 'ts' is not $gt or $gte.
assert.throws(function() {
    t.find({ts: {$lt: 4}}).addOption(DBQuery.Option.oplogReplay).next();
});
assert.throws(function() {
    t.find({ts: {$lt: 4}, _id: 3}).addOption(DBQuery.Option.oplogReplay).next();
});

// Query on just the 'ts' field.
var cursor = t.find({ts: {$gt: 20}}).addOption(DBQuery.Option.oplogReplay);
assert.eq(21, cursor.next()["_id"]);
assert.eq(22, cursor.next()["_id"]);

// Query over both 'ts' and '_id' should only pay attention to the 'ts'
// field for finding the oplog start (SERVER-13566).
cursor = t.find({ts: {$gte: 20}, _id: 25}).addOption(DBQuery.Option.oplogReplay);
assert.eq(25, cursor.next()["_id"]);
assert(!cursor.hasNext());
