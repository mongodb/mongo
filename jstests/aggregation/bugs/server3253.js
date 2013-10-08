// server-3253 Unsharded support for $out

load('jstests/aggregation/extras/utils.js');

var input = db.server3253_in;
var output = db.server3253_out;

input.drop();
output.drop();

function test(pipeline, expected) {
    pipeline.push({$out: output.getName()});

    assert.eq(input.aggregate(pipeline).itcount(), 0); // empty cursor returned
    assert.eq(output.find().toArray(), expected); // correct results
}


input.insert({_id:1});
input.insert({_id:2});
input.insert({_id:3});

db.system.namespaces.count({name: /tmp\.agg_out/});

// basic test
test([{$project: {a: {$add: ['$_id', '$_id']}}}],
     [{_id:1, a:2},{_id:2, a:4},{_id:3, a:6}]);

// test with indexes
assert.eq(output.getIndexes().length, 1);
output.ensureIndex({a:1});
assert.eq(output.getIndexes().length, 2);
test([{$project: {a: {$multiply: ['$_id', '$_id']}}}],
     [{_id:1, a:1},{_id:2, a:4},{_id:3, a:9}]);
assert.eq(output.getIndexes().length, 2);

// test with capped collection
output.drop();
db.createCollection(output.getName(), {capped: true, size: 2});
assertErrorCode(input, {$out: output.getName()}, 17152);

// shoudn't leave temp collections laying around
assert.eq([], db.system.namespaces.find({name: /tmp\.agg_out/}).toArray());
