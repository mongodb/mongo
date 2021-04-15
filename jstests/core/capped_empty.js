/**
 * @tags: [
 *   requires_capped,
 *   requires_fastcount,
 *   requires_non_retryable_commands,
 *   uses_testing_only_commands,
 *   requires_emptycapped,
 *   # capped collections connot be sharded
 *   assumes_unsharded_collection,
 *   # emptycapped command is not supported on mongos
 *   assumes_against_mongod_not_mongos,
 * ]
 */

t = db.capped_empty;
t.drop();

db.createCollection(t.getName(), {capped: true, size: 100});

t.insert({x: 1});
t.insert({x: 2});
t.insert({x: 3});
t.createIndex({x: 1});

assert.eq(3, t.count());

t.runCommand("emptycapped");

assert.eq(0, t.count());

t.insert({x: 1});
t.insert({x: 2});
t.insert({x: 3});

assert.eq(3, t.count());
