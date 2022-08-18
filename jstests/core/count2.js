// @tags: [requires_getmore, requires_fastcount]
(function() {
"use strict";
const coll = db.count2;
coll.drop();

for (var i = 0; i < 1000; i++) {
    assert.commandWorked(coll.insert({num: i, m: i % 20}));
}

assert.eq(1000, coll.count());
assert.eq(1000, coll.find().count());
assert.eq(1000, coll.find().toArray().length);

assert.eq(50, coll.find({m: 5}).toArray().length);
assert.eq(50, coll.find({m: 5}).count());

assert.eq(40, coll.find({m: 5}).skip(10).toArray().length);
assert.eq(50, coll.find({m: 5}).skip(10).count());
assert.eq(40, coll.find({m: 5}).skip(10).count(true));

assert.eq(20, coll.find({m: 5}).skip(10).limit(20).toArray().length);
assert.eq(50, coll.find({m: 5}).skip(10).limit(20).count());
assert.eq(20, coll.find({m: 5}).skip(10).limit(20).count(true));

assert.eq(5, coll.find({m: 5}).skip(45).limit(20).count(true));

// Negative skip values should return error
var negSkipResult = db.runCommand({count: coll.getName(), skip: -2});
assert(!negSkipResult.ok, "negative skip value shouldn't work, n = " + negSkipResult.n);
assert(negSkipResult.errmsg.length > 0, "no error msg for negative skip");
}());
