// Check that shard selection does not assert for certain unsatisfiable queries.
// SERVER-4554, SERVER-4914

s = new ShardingTest({name: 'shard7', shards: 2});

db = s.admin._mongo.getDB('test');
c = db['foo'];
c.drop();

s.adminCommand({enablesharding: '' + db});
s.ensurePrimaryShard(db.getName(), 'shard0001');
s.adminCommand({shardcollection: '' + c, key: {a: 1, b: 1}});

// Check query operation with some satisfiable and unsatisfiable queries.

assert.eq(0, c.find({a: 1}).itcount());
assert.eq(0, c.find({a: 1, b: 1}).itcount());
assert.eq(0, c.find({a: {$gt: 4, $lt: 2}}).itcount());
assert.eq(0, c.find({a: 1, b: {$gt: 4, $lt: 2}}).itcount());
assert.eq(0, c.find({a: {$gt: 0, $lt: 2}, b: {$gt: 4, $lt: 2}}).itcount());
assert.eq(0, c.find({b: {$gt: 4, $lt: 2}}).itcount());
assert.eq(0, c.find({a: {$in: []}}).itcount());
assert.eq(0, c.find({a: 1, b: {$in: []}}).itcount());

assert.eq(0, c.find({$or: [{a: {$gt: 0, $lt: 10}}, {a: 12}]}).itcount());
assert.eq(0, c.find({$or: [{a: {$gt: 0, $lt: 10}}, {a: 5}]}).itcount());
assert.eq(0, c.find({$or: [{a: 1, b: {$gt: 0, $lt: 10}}, {a: 1, b: 5}]}).itcount());

// Check other operations that use getShardsForQuery.

unsatisfiable = {
    a: 1,
    b: {$gt: 4, $lt: 2}
};

assert.eq(0, c.count(unsatisfiable));
assert.eq([], c.distinct('a', unsatisfiable));

aggregate = c.aggregate({$match: unsatisfiable});
assert.eq(0, aggregate.toArray().length);

c.save({a: null, b: null});
c.save({a: 1, b: 1});
assert.writeOK(c.remove(unsatisfiable));
assert.eq(2, c.count());
assert.writeOK(c.update(unsatisfiable, {$set: {c: 1}}, false, true));
assert.eq(2, c.count());
assert.eq(0, c.count({c: 1}));

c.ensureIndex({loc: '2d'});
c.save({a: 2, b: 2, loc: [0, 0]});
near = db.runCommand({geoNear: 'foo', near: [0, 0], query: unsatisfiable});
assert.commandWorked(near);
assert.eq(0, near.results.length);
