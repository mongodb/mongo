// server-3253 Unsharded support for $out
load('jstests/aggregation/extras/utils.js'); // assertErrorCode
load("jstests/libs/fts.js"); // used for testing text index

load('jstests/aggregation/extras/utils.js');

var input = db.server3253_in;
var output = db.server3253_out;

input.drop();
output.drop();

function getOutputIndexes() {
    return db.system.indexes.find({ns: output.getFullName()}).sort({"key":1}).toArray();
}

function test(pipeline, expected) {
    pipeline.push({$out: output.getName()});
    var indexes = getOutputIndexes();

    var cursor = input.aggregate(pipeline);

    assert.eq(cursor.itcount(), 0); // empty cursor returned
    assert.eq(output.find().toArray(), expected); // correct results
    assert.eq(getOutputIndexes(), indexes); // number of indexes maintained
}


input.insert({_id:1});
input.insert({_id:2});
input.insert({_id:3});

// insert into output so that the index exists and test() does not fail the first time around
output.insert({_id:1});

// ensure there are no tmp agg_out collections before we begin
assert.eq([], db.system.namespaces.find({name: /tmp\.agg_out/}).toArray());

// basic test
test([{$project: {a: {$add: ['$_id', '$_id']}}}],
     [{_id:1, a:2},{_id:2, a:4},{_id:3, a:6}]);

// test with indexes
assert.eq(output.getIndexes().length, 1);
output.ensureIndex({a:1});
assert.eq(output.getIndexes().length, 2);
test([{$project: {a: {$multiply: ['$_id', '$_id']}}}],
     [{_id:1, a:1},{_id:2, a:4},{_id:3, a:9}]);

// test with empty result set and make sure old result is gone, but indexes remain
test([{$match: {_id: 11}}],
     []);
assert.eq(output.getIndexes().length, 2);

// test with geo index
output.ensureIndex({b:"2d"});
assert.eq(output.getIndexes().length, 3);
test([{$project: {b: "$_id"}}],
     [{_id:1, b:1}, {_id:2, b:2}, {_id:3, b:3}]);

// test with full text index
output.ensureIndex({c:"text"});
assert.eq(output.getIndexes().length, 4);
test([{$project: {c: {$concat: ["hello there ", "_id"]}}}],
     [{_id:1, c:"hello there _id"}, {_id:2, c:"hello there _id"}, {_id:3, c:"hello there _id"}]);

// test with capped collection
output.drop();
db.createCollection(output.getName(), {capped: true, size: 2});
assertErrorCode(input, {$out: output.getName()}, 17152);

// ensure we cant do dangerous things to system
output = db.getSiblingDB("system").server3253_out;
input = db.getSiblingDB("system").server3253_in;
assertErrorCode(input, {$out: output.getName()}, 16994);

// shoudn't leave temp collections laying around
assert.eq([], db.system.namespaces.find({name: /tmp\.agg_out/}).toArray());
