/**
 * Validates that runtime changes to batched write oplog limits affect how many applyOps entries are
 * generated.
 *
 * @tags: [requires_fcv_80, requires_replication]
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({
    nodes: [
        {
            setParameter: {
                featureFlagLargeBatchedOperations: true,
                // Keep batched deletes together so the limits we set drive the splitting.
                batchedDeletesTargetBatchTimeMS: 0,
            },
        },
    ],
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const db = primary.getDB("test");
const coll = db.batched_write_runtime_limits;

const oplogQuery = {
    op: "c",
    ns: "admin.$cmd",
    "o.applyOps": {$elemMatch: {ns: coll.getFullName()}},
};

function latestOplogTs() {
    const last = primary.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).toArray();
    return last.length ? last[0].ts : Timestamp(0, 0);
}

function getApplyOpsSince(ts) {
    return rst.findOplog(primary, Object.assign({ts: {$gt: ts}}, oplogQuery)).toArray();
}

// Baseline: default limits produce a single applyOps entry.
assertDropAndRecreateCollection(db, coll.getName());
assert.commandWorked(coll.insert([{_id: 0}, {_id: 1}, {_id: 2}, {_id: 3}]));
let beforeTs = latestOplogTs();
assert.commandWorked(coll.remove({}));
let ops = getApplyOpsSince(beforeTs);
assert.eq(1, ops.length, () => `expected a single applyOps entry, got: ${tojson(ops)}`);

// Lower the op count limit and expect the delete to split across two applyOps.
assert.commandWorked(primary.adminCommand({setParameter: 1, maxNumberOfBatchedOperationsInSingleOplogEntry: 2}));
assertDropAndRecreateCollection(db, coll.getName());
assert.commandWorked(coll.insert([{_id: 10}, {_id: 11}, {_id: 12}, {_id: 13}]));
beforeTs = latestOplogTs();
assert.commandWorked(coll.remove({}));
ops = getApplyOpsSince(beforeTs);
assert.eq(2, ops.length, () => `expected two applyOps entries after lowering op-count limit: ${tojson(ops)}`);

// Lower the size limit and expect a split driven by size.
const bigString = "x".repeat(400);
assert.commandWorked(primary.adminCommand({setParameter: 1, maxNumberOfBatchedOperationsInSingleOplogEntry: 100}));
assert.commandWorked(primary.adminCommand({setParameter: 1, maxSizeOfBatchedOperationsInSingleOplogEntryBytes: 200}));
assertDropAndRecreateCollection(db, coll.getName());
assert.commandWorked(
    coll.insert([
        {_id: 20, payload: bigString},
        {_id: 21, payload: bigString},
    ]),
);
beforeTs = latestOplogTs();
assert.commandWorked(coll.remove({}));
ops = getApplyOpsSince(beforeTs);
assert.eq(2, ops.length, () => `expected two applyOps entries after lowering size limit: ${tojson(ops)}`);

rst.stopSet();
