/**
 * Tests bulkWrite-specific reply syntax when performing a shard key update that causes a document
 * to change shards.
 *
 * @tags: [
 *  requires_fcv_80
 * ]
 */

import {
    cursorEntryValidator,
    cursorSizeValidator,
    summaryFieldsValidator
} from "jstests/libs/bulk_write_utils.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    shardCollectionMoveChunks,
} from "jstests/sharding/libs/update_shard_key_helpers.js";

const st = new ShardingTest({
    mongos: 1,
    shards: {rs0: {nodes: 3}, rs1: {nodes: 3}},
    rsOptions:
        {setParameter: {maxTransactionLockRequestTimeoutMillis: ReplSetTest.kDefaultTimeoutMS}}
});

const kDbName = 'db';
const mongos = st.s0;
const shard0 = st.shard0.shardName;
const ns = kDbName + '.foo';

assert.commandWorked(mongos.adminCommand({enableSharding: kDbName, primaryShard: shard0}));

const adminDB = st.s.getDB("admin");

[false, true].forEach(function(upsert) {
    st.s.getDB(kDbName).foo.drop();

    jsTestLog("Testing with upsert=" + upsert);

    const docsToInsert = upsert
        ? [{"x": 1}, {"x": 100}]
        : [{"x": 4, "a": 3}, {"x": 100}, {"x": 300, "a": 3}, {"x": 500, "a": 6}];

    const splitDoc = {"x": 100};

    // Shard the collection and ensure the chunk with x:300 and x:500 ends up on shard1.
    shardCollectionMoveChunks(st, kDbName, ns, {"x": 1}, docsToInsert, splitDoc, {"x": 300});

    // Run an update that should move the x:300 doc to the other shard (or, in the upsert case,
    // upsert the document on a different shard than the one where the doc would have lived
    // prior to the update.)
    let mods = upsert ? {$set: {x: 30, _id: 1}} : {$set: {x: 30}};
    let res = assert.commandWorked(adminDB.runCommand({
        bulkWrite: 1,
        ops: [{update: 0, filter: {x: 300}, updateMods: mods, upsert: upsert}],
        nsInfo: [{ns: ns}],
        lsid: {"id": UUID()},
        txnNumber: NumberLong(1),
    }));

    cursorSizeValidator(res, 1);
    if (upsert) {
        cursorEntryValidator(res.cursor.firstBatch[0],
                             {ok: 1, idx: 0, n: 1, nModified: 0, upserted: {_id: 1}});
        summaryFieldsValidator(
            res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1});
    } else {
        cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});
        summaryFieldsValidator(
            res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});
    }

    // Run a similar update for the x:4 document, to test moving in the opposite direction
    // between the shards.
    mods = upsert ? {$set: {x: 600, _id: 2}} : {$set: {x: 600}};
    res = assert.commandWorked(adminDB.runCommand({
        bulkWrite: 1,
        ops: [{update: 0, filter: {x: 4}, updateMods: mods, upsert: upsert}],
        nsInfo: [{ns: ns}],
        lsid: {"id": UUID()},
        txnNumber: NumberLong(1),
    }));

    cursorSizeValidator(res, 1);
    if (upsert) {
        cursorEntryValidator(res.cursor.firstBatch[0],
                             {ok: 1, idx: 0, n: 1, nModified: 0, upserted: {_id: 2}});
        summaryFieldsValidator(
            res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 1});
    } else {
        cursorEntryValidator(res.cursor.firstBatch[0], {ok: 1, idx: 0, n: 1, nModified: 1});
        summaryFieldsValidator(
            res, {nErrors: 0, nInserted: 0, nDeleted: 0, nMatched: 1, nModified: 1, nUpserted: 0});
    }

    // For the purpose of this test case upsert being true/false is irrelevant so we only run it
    // once.
    if (!upsert) {
        // Run an update that, while it would change a document's owning shard, won't end up being
        // executed because it hasn't been sent in its own batch. This is to test that we *don't*
        // adjust the result fields in the event we didn't actually perform the update.
        const res = assert.commandWorked(adminDB.runCommand({
            bulkWrite: 1,
            ops: [
                {update: 0, filter: {x: 600}, updateMods: {$set: {x: 4}}},
                {update: 0, filter: {x: 1}, updateMods: {$set: {y: 2}}}
            ],
            nsInfo: [{ns: ns}],
            lsid: {"id": UUID()},
            txnNumber: NumberLong(1),
        }));

        cursorSizeValidator(res, 1);
        cursorEntryValidator(res.cursor.firstBatch[0],
                             {ok: 0, idx: 0, code: ErrorCodes.InvalidOptions, n: 0, nModified: 0});
        summaryFieldsValidator(
            res, {nErrors: 1, nInserted: 0, nDeleted: 0, nMatched: 0, nModified: 0, nUpserted: 0});
    }
});

mongos.getDB(kDbName).foo.drop();

st.stop();
