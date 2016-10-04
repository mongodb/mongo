// Test queries that set the OplogReplay flag.

function test(t) {
    t.drop();
    assert.commandWorked(t.getDB().createCollection(t.getName(), {capped: true, size: 16 * 1024}));

    function makeTS(i) {
        return Timestamp(1000, i);
    }

    for (var i = 0; i < 100; i++) {
        t.save({_id: i, ts: makeTS(i)});
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
        t.find({$or: [{ts: {$gt: makeTS(3)}}, {foo: 3}]})
            .addOption(DBQuery.Option.oplogReplay)
            .next();
    });
    assert.throws(function() {
        t.find({$nor: [{ts: {$gt: makeTS(4)}}, {foo: 4}]})
            .addOption(DBQuery.Option.oplogReplay)
            .next();
    });

    // Predicate over 'ts' is not $gt or $gte.
    assert.throws(function() {
        t.find({ts: {$lt: makeTS(4)}}).addOption(DBQuery.Option.oplogReplay).next();
    });
    assert.throws(function() {
        t.find({ts: {$lt: makeTS(4)}, _id: 3}).addOption(DBQuery.Option.oplogReplay).next();
    });

    // Query on just the 'ts' field.
    var cursor = t.find({ts: {$gt: makeTS(20)}}).addOption(DBQuery.Option.oplogReplay);
    assert.eq(21, cursor.next()["_id"]);
    assert.eq(22, cursor.next()["_id"]);

    // Query over both 'ts' and '_id' should only pay attention to the 'ts'
    // field for finding the oplog start (SERVER-13566).
    cursor = t.find({ts: {$gte: makeTS(20)}, _id: 25}).addOption(DBQuery.Option.oplogReplay);
    assert.eq(25, cursor.next()["_id"]);
    assert(!cursor.hasNext());
}

// test on non-oplog
test(db.jstests_query_oplogreplay);

// test on real oplog
test(db.getSiblingDB('local').oplog.jstests_query_oplogreplay);

// test on non-capped collection
var coll = db.jstests_query_oplogreplay;
coll.drop();
assert.commandWorked(coll.getDB().createCollection(coll.getName()));
var res = assert.throws(function() {
    coll.find({ts: {$gt: "abcd"}}).addOption(DBQuery.Option.oplogReplay).next();
});
