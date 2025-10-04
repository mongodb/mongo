// @tags: [ requires_sharding ]
// Jstests rely on the shell eval option, implicitly_shard_accessed_collections.js, to override
// any command that calls DB.prototype.getCollection() with function that attempts to shard
// the collection before returning it. This test confirms that this sharding test override works
// for sharded deployments with mongot enabled. In other words, this test confirms that the
// collections in sharded search e2e suite's tests are indeed sharded.

const outputColl = db.out;
const inputColl = db.input;
inputColl.drop();
outputColl.drop();

inputColl.insert({a: -1});
inputColl.insert({a: -10});
inputColl.insert({a: 100});
outputColl.insert({a: -1000});
outputColl.insert({a: 1000});
outputColl.insert({a: 1});

// You cannot specify a sharded collection as the output collection, so this should throw if
// collection is indeed sharded.
assert.throws(() => {
    inputColl.aggregate([{$group: {_id: "$_id", sum: {$sum: "$a"}}}, {$out: outputColl.getName()}]);
});
