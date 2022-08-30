/**
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
(function() {
"use strict";

const dbName = jsTestName();
const setupShardedCluster = (shards = 1) => {
    const st = new ShardingTest(
        {shards, mongos: 1, config: 1, rs: {nodes: 1, setParameter: {writePeriodicNoops: false}}});
    const sdb = st.s0.getDB(dbName);
    assert.commandWorked(sdb.dropDatabase());

    sdb.setProfilingLevel(0, -1);
    st.shard0.getDB(dbName).setProfilingLevel(0, -1);

    // Shard the relevant collections.
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);
    if (shards === 2) {
        // Shard the collection on {_id: 1}, split at {_id: 0} and move the empty upper chunk to
        // shard1.
        st.shardColl("coll", {_id: 1}, {_id: 0}, {_id: 0}, dbName);
        st.shardColl("coll2", {_id: 1}, {_id: 0}, {_id: 0}, dbName);
    } else {
        assert(shards === 1, "only 1 or 2 shards supported");
        assert.commandWorked(st.s.adminCommand({shardCollection: dbName + ".coll", key: {_id: 1}}));
        assert.commandWorked(
            st.s.adminCommand({shardCollection: dbName + ".coll2", key: {_id: 1}}));
    }

    const shardId = st.shard0.shardName;
    return [sdb, st, shardId];
};

const pscWatch = (db, coll, shardId, options = {}, csOptions = {}) => {
    let cmd = {
        aggregate: coll,
        cursor: {},
        pipeline: [{$changeStream: csOptions}],
        $_passthroughToShard: {shard: shardId}
    };
    cmd = Object.assign({}, cmd, options);
    if (options.pipeline) {
        cmd.pipeline = [{$changeStream: csOptions}].concat(options.pipeline);
    }
    const resp = db.runCommand(cmd);
    assert.commandWorked(resp);
    if (options.explain) {
        return resp;
    }
    return new DBCommandCursor(db, resp);
};

// Parsing
let [sdb, st, shardId] = setupShardedCluster();

// Should not allow pipeline without $changeStream.
assert.commandFailedWithCode(sdb.runCommand({
    aggregate: "coll",
    cursor: {},
    pipeline: [{$match: {perfect: true}}],
    $_passthroughToShard: {shard: shardId}
}),
                             6273801);

// $out can't passthrough so it's not allowed.
assert.commandFailedWithCode(
    assert.throws(() => pscWatch(sdb, "coll", shardId, {pipeline: [{$out: "h"}]})), 6273802);

// Shard option should be specified.
assert.commandFailedWithCode(
    sdb.runCommand(
        {aggregate: "coll", cursor: {}, pipeline: [{$changeStream: {}}], $_passthroughToShard: {}}),
    40414);

// The shardId field should be a string.
assert.commandFailedWithCode(assert.throws(() => pscWatch(sdb, "coll", 42)),
                                          ErrorCodes.TypeMismatch);
// Can't open a per shard cursor on the config RS.
assert.commandFailedWithCode(assert.throws(() => pscWatch(sdb, "coll", "config")), 6273803);

// The shardId should be a valid shard.
assert.commandFailedWithCode(
    assert.throws(() => pscWatch(sdb, "coll", "Dwane 'the Shard' Johnson")),
                 ErrorCodes.ShardNotFound);

// Correctness.

// Simple collection level watch
// this insert shouldn't show up since it happens before we make a cursor.
sdb.coll.insertOne({location: 1});
let c = pscWatch(sdb, "coll", shardId);
// these inserts should show up since they're after we make a cursor.
for (let i = 1; i <= 4; i++) {
    sdb.coll.insertOne({location: 2, i});
    assert(!c.isExhausted());
    assert(c.hasNext());
    c.next();
}
assert(!c.hasNext());

// Simple database level watch
c = pscWatch(sdb, 1, shardId);

sdb.coll.insertOne({location: 3});
assert(!c.isExhausted());
assert(c.hasNext());
c.next();

sdb.coll2.insertOne({location: 4});
assert(!c.isExhausted());
assert(c.hasNext());
c.next();

assert(!c.hasNext());

// Watching collection that doesn't exist yet.
c = pscWatch(sdb, "toBeCreated", shardId);
assert(!c.isExhausted());
assert(!c.hasNext());

st.s.adminCommand({shardCollection: dbName + ".toBeCreated", key: {_id: 1}});
assert(!c.isExhausted());
assert(!c.hasNext());

sdb.toBeCreated.insertOne({location: 8});
assert(!c.isExhausted());
assert(c.hasNext());
c.next();

assert(!c.hasNext());

// Explain output should not have a split pipeline. It should look like mongod explain output.
let explainOut = pscWatch(sdb, "coll", shardId, {explain: true});
assert(!explainOut.hasOwnProperty("splitPipeline"));
assert.hasOwnProperty(explainOut, "stages");

// If we getMore an invalidated cursor the cursor should have been closed on mongos and we should
// get CursorNotFound, even if the invalidate event was never received by mongos.
[[], [{$match: {f: "filter out invalidate event"}}]].forEach((pipeline) => {
    assert.commandWorked(st.s.adminCommand({shardCollection: dbName + ".toDrop", key: {_id: 1}}));
    let c = pscWatch(sdb, "toDrop", shardId, {pipeline});
    sdb.toDrop.insertOne({});
    sdb.toDrop.drop();
    assert.commandFailedWithCode(
        assert.throws(() => {
                         assert.retry(() => {
                             c._runGetMoreCommand();
                             return false;
                         }, "change stream should have been invalidated by now", 4);
                     }),
                     ErrorCodes.CursorNotFound);
});

st.stop();

// Isolated from events on other shards.
[sdb, st, shardId] = setupShardedCluster(2);
c = pscWatch(sdb, "coll", shardId);

sdb.coll.insertOne({location: 5, _id: -2});
assert(!c.isExhausted());
assert(c.hasNext());
c.next();

sdb.coll.insertOne({location: 6, _id: 2});
assert(!c.isExhausted());
assert(!c.hasNext());

// Isolated from events on other shards with whole db.
c = pscWatch(sdb.getSiblingDB("admin"), 1, shardId, {}, {allChangesForCluster: true});

sdb.coll.insertOne({location: 7, _id: -3});
assert(!c.isExhausted());
assert(c.hasNext());
c.next();

sdb.coll2.insertOne({location: 8, _id: -4});
assert(!c.isExhausted());
assert(c.hasNext());
c.next();

sdb.coll.insertOne({location: 9, _id: 3});
assert(!c.isExhausted());
assert(!c.hasNext());

sdb.coll2.insertOne({location: 10, _id: 4});
assert(!c.isExhausted());
assert(!c.hasNext());

st.stop();
})();
