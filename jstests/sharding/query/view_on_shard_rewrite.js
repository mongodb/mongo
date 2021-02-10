/**
 * When a view is defined on a sharded collection, read of that view results in the following:
 *  1) mongos executes the view read against the primary shard, agnostic to whether the read
 *     is on a view or an unsharded collection.
 *  2) The primary shard resolves the view and on determining that the underlying collection is
 *     sharded, returns an 'CommandOnShardedViewNotSupportedOnMongod' error to mongos, with the
 *     resolved view definition attached.
 *  3) mongos rewrites the query and resolved view definition to be an aggregate on the underlying
 *     collection and executes.
 *
 * This test confirms that this rewrite mechanism is only used when the underlying collection is
 * sharded. If not sharded then the primary shard can perform the rewrite locally and execute
 * without having to round-trip back to mongos.
 *
 * @tags: [requires_find_command, requires_fcv_49]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

const dbName = "view_on_shard_rewrite";
const collName = "coll";
const viewName = "view";
const collNS = dbName + "." + collName;
const mongos = st.s0;
const configDB = mongos.getDB("config");
const mongosDB = mongos.getDB(dbName);

const shardPrimary = st.rs0.getPrimary();
const shardDB = shardPrimary.getDB(dbName);

assert.commandWorked(mongosDB.getCollection(collName).insert([{_id: 1}, {_id: 2}, {_id: 3}]));
assert.commandWorked(mongosDB.createView(viewName, collName, [{$addFields: {foo: "$_id"}}]));

function assertReadOnView(view) {
    const result = view.find({}).sort({_id: 1}).toArray();
    assert.eq(result, [{_id: 1, foo: 1}, {_id: 2, foo: 2}, {_id: 3, foo: 3}]);
}

// View read with unsharded collection works on both the primary shard and mongos.
assertReadOnView(mongosDB.getCollection(viewName));
assertReadOnView(shardDB.getCollection(viewName));

assert.commandWorked(configDB.adminCommand({enableSharding: dbName}));

// View read with unsharded collection works on both the primary shard and mongos, even when
// sharding has been enabled on the database.
assertReadOnView(mongosDB.getCollection(viewName));
assertReadOnView(shardDB.getCollection(viewName));

assert.commandWorked(configDB.adminCommand({shardCollection: collNS, key: {_id: 1}}));

// View read with sharded collection works on mongos.
assertReadOnView(mongosDB.getCollection(viewName));

// View read with sharded collection on the primary shard is rejected. This mimics what happens
// when mongos sends the equivalent read, with the caveat that mongos will use the view definition
// returned to rewrite the query and execute against the underlying collection.
const result = assert.throws(() => shardDB.getCollection(viewName).findOne({}));
assert.eq(result.code, ErrorCodes.CommandOnShardedViewNotSupportedOnMongod);

st.stop();
})();
