/**
 * Tests that bulkWrite correctly retries on stale errors.
 *
 * @tags: [
 *   requires_fcv_83,
 * ]
 */
import {cursorEntryValidator, cursorSizeValidator, summaryFieldsValidator} from "jstests/libs/bulk_write_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    mongos: 2,
    shards: 1,
    rs: {nodes: 1},
});

const dbName = "test";
const unsplittableCollName = "unsplittable_coll";
const unshardedCollName = "unsharded_coll";
const unsplittableNs = `${dbName}.${unsplittableCollName}`;
const unshardedNs = `${dbName}.${unshardedCollName}`;

const db0 = st.s0.getDB(dbName);
const db1 = st.s1.getDB(dbName);

// Run a sharding command to establish a database version on mongos 1.
assert.commandWorked(
    db1.runCommand({createUnsplittableCollection: unsplittableCollName, dataShard: st.shard0.shardName}),
);

// Recreated the database to update the database version on mongos 0.
db0.dropDatabase();
assert.commandWorked(
    db0.runCommand({createUnsplittableCollection: unsplittableCollName, dataShard: st.shard0.shardName}),
);
assert.commandWorked(db0.createCollection(unshardedCollName));

// Execute a bulkWrite on mongos 1 to ensure it refreshes database and shard versions correctly.
const res = assert.commandWorked(
    db1.adminCommand({
        bulkWrite: 1,
        ops: [
            {insert: 0, document: {skey: "MongoDB", type: "unsplittable"}},
            {insert: 1, document: {skey: "MongoDB", type: "unsharded"}},
        ],
        nsInfo: [{ns: unsplittableNs}, {ns: unshardedNs}],
    }),
);

// Validate the bulkWrite response.
cursorSizeValidator(res, 2);
summaryFieldsValidator(res, {nErrors: 0, nInserted: 2, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});
cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, n: 1, idx: 0});
cursorEntryValidator(res.cursor.firstBatch[1], {ok: 1, n: 1, idx: 1});

const unsplittableDocs = db0[unsplittableCollName].find().toArray();
assert.eq(unsplittableDocs.length, 1, "Expected 1 document in unsplittable collection");
assert.eq(unsplittableDocs[0].skey, "MongoDB");
assert.eq(unsplittableDocs[0].type, "unsplittable");

const unshardedDocs = db0[unshardedCollName].find().toArray();
assert.eq(unshardedDocs.length, 1, "Expected 1 document in unsharded collection");
assert.eq(unshardedDocs[0].skey, "MongoDB");
assert.eq(unshardedDocs[0].type, "unsharded");

st.stop();
