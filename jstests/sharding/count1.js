(function() {
'use strict';

let s = new ShardingTest({shards: 2});
let db = s.getDB("test");

// ************** Test Set #1 *************
// Basic counts on "bar" collections, not yet sharded.
assert.commandWorked(db.bar.insert([{n: 1}, {n: 2}, {n: 3}]));

assert.eq(3, db.bar.find().count());
assert.eq(1, db.bar.find({n: 1}).count());

//************** Test Set #2 *************
// Basic counts on sharded  "foo" collection.

// 1. Create foo collection, insert 6 docs.
s.adminCommand({enablesharding: "test"});
s.ensurePrimaryShard('test', s.shard1.shardName);
s.adminCommand({shardcollection: "test.foo", key: {name: 1}});

let primary = s.getPrimaryShard("test").getDB("test");
let secondary = s.getOther(primary).getDB("test");

assert.commandWorked(db.foo.insert({_id: 1, name: "eliot"}));
assert.commandWorked(db.foo.insert({_id: 2, name: "sara"}));
assert.commandWorked(db.foo.insert({_id: 3, name: "bob"}));
assert.commandWorked(db.foo.insert({_id: 4, name: "joe"}));
assert.commandWorked(db.foo.insert({_id: 5, name: "mark"}));
assert.commandWorked(db.foo.insert({_id: 6, name: "allan"}));

assert.eq(6, db.foo.find().count());

// 2. Divide into three chunks.
assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {name: "allan"}}));
assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {name: "sara"}}));
assert.commandWorked(s.s0.adminCommand({split: "test.foo", middle: {name: "eliot"}}));

// 3. Test counts before chunk migrations.
assert.eq(6, db.foo.find().count());
assert.eq(6, db.foo.find().sort({name: 1}).count());

// 4. Manually move chunks.  Now each shard should have 3 docs.
assert.commandWorked(s.s0.adminCommand({
    moveChunk: "test.foo",
    find: {name: "eliot"},
    to: secondary.getMongo().name,
    _waitForDelete: true
}));

assert.eq(3, primary.foo.find().toArray().length);
assert.eq(3, secondary.foo.find().toArray().length);
assert.eq(3, primary.foo.find().sort({name: 1}).toArray().length);
assert.eq(3, secondary.foo.find().sort({name: 1}).toArray().length);

// 5. Some redundant tests, but better safe than sorry. These are fast tests, anyway.

// i. Test basic counts on foo.
assert.eq(6, db.foo.find().count());
assert.eq(6, db.foo.find().toArray().length);
assert.eq(6, db.foo.find().sort({name: 1}).toArray().length);
assert.eq(6, db.foo.find().sort({name: 1}).count());

// ii. Test counts with limit.
assert.eq(2, db.foo.find().limit(2).count(true));
assert.eq(2, db.foo.find().limit(-2).count(true));
assert.eq(6, db.foo.find().limit(100).count(true));
assert.eq(6, db.foo.find().limit(-100).count(true));
assert.eq(6, db.foo.find().limit(0).count(true));

// iii. Test counts with skip.
assert.eq(6, db.foo.find().skip(0).count(true));
assert.eq(5, db.foo.find().skip(1).count(true));
assert.eq(4, db.foo.find().skip(2).count(true));
assert.eq(3, db.foo.find().skip(3).count(true));
assert.eq(2, db.foo.find().skip(4).count(true));
assert.eq(1, db.foo.find().skip(5).count(true));
assert.eq(0, db.foo.find().skip(6).count(true));
assert.eq(0, db.foo.find().skip(7).count(true));

// iv. Test counts with skip + limit.
assert.eq(2, db.foo.find().limit(2).skip(1).count(true));
assert.eq(2, db.foo.find().limit(-2).skip(1).count(true));
assert.eq(5, db.foo.find().limit(100).skip(1).count(true));
assert.eq(5, db.foo.find().limit(-100).skip(1).count(true));
assert.eq(5, db.foo.find().limit(0).skip(1).count(true));

assert.eq(0, db.foo.find().limit(2).skip(10).count(true));
assert.eq(0, db.foo.find().limit(-2).skip(10).count(true));
assert.eq(0, db.foo.find().limit(100).skip(10).count(true));
assert.eq(0, db.foo.find().limit(-100).skip(10).count(true));
assert.eq(0, db.foo.find().limit(0).skip(10).count(true));

assert.eq(2, db.foo.find().limit(2).itcount());
assert.eq(2, db.foo.find().skip(2).limit(2).itcount());
assert.eq(1, db.foo.find().skip(5).limit(2).itcount());
assert.eq(6, db.foo.find().limit(2).count());
assert.eq(2, db.foo.find().limit(2).size());
assert.eq(2, db.foo.find().skip(2).limit(2).size());
assert.eq(1, db.foo.find().skip(5).limit(2).size());
assert.eq(4, db.foo.find().skip(1).limit(4).size());
assert.eq(5, db.foo.find().skip(1).limit(6).size());

// SERVER-3567 older negative limit tests
assert.eq(2, db.foo.find().limit(2).itcount());
assert.eq(2, db.foo.find().limit(-2).itcount());
assert.eq(2, db.foo.find().skip(4).limit(2).itcount());
assert.eq(2, db.foo.find().skip(4).limit(-2).itcount());

// v. Test counts with skip + limit + sorting.
function nameString(c) {
    let s = "";
    while (c.hasNext()) {
        let o = c.next();
        if (s.length > 0)
            s += ",";
        s += o.name;
    }
    return s;
}
assert.eq("allan,bob,eliot,joe,mark,sara", nameString(db.foo.find().sort({name: 1})));
assert.eq("sara,mark,joe,eliot,bob,allan", nameString(db.foo.find().sort({name: -1})));

assert.eq("allan,bob", nameString(db.foo.find().sort({name: 1}).limit(2)));
assert.eq("bob,eliot", nameString(db.foo.find().sort({name: 1}).skip(1).limit(2)));
assert.eq("joe,mark", nameString(db.foo.find().sort({name: 1}).skip(3).limit(2)));

assert.eq("eliot,sara", nameString(db.foo.find().sort({_id: 1}).limit(2)));
assert.eq("sara,bob", nameString(db.foo.find().sort({_id: 1}).skip(1).limit(2)));
assert.eq("joe,mark", nameString(db.foo.find().sort({_id: 1}).skip(3).limit(2)));

// 6. Insert 10 more docs. Further limit/skip testing with a find query.
for (let i = 0; i < 10; i++) {
    assert.commandWorked(db.foo.insert({_id: 7 + i, name: "zzz" + i}));
}

assert.eq(10, db.foo.find({name: {$gt: "z"}}).itcount());
assert.eq(10, db.foo.find({name: {$gt: "z"}}).sort({_id: 1}).itcount());
assert.eq(5, db.foo.find({name: {$gt: "z"}}).sort({_id: 1}).skip(5).itcount());
assert.eq(3, db.foo.find({name: {$gt: "z"}}).sort({_id: 1}).skip(5).limit(3).itcount());

// 7. Test invalid queries/values.

// i. Make sure count command returns error for invalid queries.
assert.commandFailedWithCode(db.runCommand({count: 'foo', query: {$c: {$abc: 3}}}),
                             ErrorCodes.BadValue);

// ii. Negative skip values should return error.
assert.commandFailedWithCode(db.runCommand({count: 'foo', skip: -2}), ErrorCodes.FailedToParse);

// iii. Negative skip values with positive limit should return error.
assert.commandFailedWithCode(db.runCommand({count: 'foo', skip: -2, limit: 1}),
                             ErrorCodes.FailedToParse);

// iv. Unknown options should return error.
assert.commandFailedWithCode(db.runCommand({count: 'foo', random: true}), 40415);

// v. Unknown options should return error for explain.
assert.commandFailedWithCode(db.runCommand({explain: {count: 'foo', random: true}}), 40415);

s.stop();
})();
