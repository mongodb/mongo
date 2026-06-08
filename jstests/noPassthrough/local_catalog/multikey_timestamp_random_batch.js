/**
 * Static-workload test that replays explicit, checked-in operation logs (single inserts/updates,
 * insertMany batches, updateMany batches, and wildcard writes) against indexes on the primary while
 * the secondary's oplog application is paused. It then resumes replication and asserts that the
 * $listCatalog snapshot around the exact oplog entry that first made each index path multikey matches
 * between the primary and the secondary. Each operation entry declares `isFirstMultikey` so the
 * expected transition timestamp is taken directly from the static log rather than inferred at
 * runtime. The wildcard cases assert that timestamped multikey metadata keys are queryable at the
 * write timestamp and not before. Catches regressions where the secondary's deferred multikey writes
 * diverge from the primary's per-timestamp catalog/index metadata state under multi-worker apply.
 *
 * Oplog shape notes (see insertDocumentsAtomically in write_ops_exec.cpp):
 *   - A multi-document insert is grouped into a single atomic applyOps oplog entry with one
 *     timestamp, so the whole batch (and therefore the multikey transition it causes) lands at that
 *     single timestamp.
 *   - An updateMany produces one update oplog entry per matched document, each with its own
 *     timestamp, so the first updated document is the exact entry that makes the index multikey.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_replication,
 *   requires_snapshot_read,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {restartServerReplication} from "jstests/libs/write_concern_util.js";

////////////////////////////////////////////////////////////////////////////////
// Test configuration.
////////////////////////////////////////////////////////////////////////////////

const kAbsentMultikeyPath = BinData(0, "AA=="); // bytes: [0] -> {}
const kPathComponentZero = BinData(0, "AQ=="); // bytes: [1] -> {0}
const kAbsentTwoComponentMultikeyPath = BinData(0, "AAA="); // bytes: [0, 0] -> {}
const kTwoComponentPathComponentZero = BinData(0, "AQA="); // bytes: [1, 0] -> {0}

const dbName = jsTestName();

// Each btree case carries a literal list of seed documents and a static operation log. Every
// operation that should be the first write to make an index path multikey is marked
// `isFirstMultikey: true`, which is the reference the assertions use for that path's expected
// transition timestamp. Across the cases below every operation family (insert, update, insertMany,
// updateMany) is the first multikey transition for at least one path.
const kBtreeCases = [
    {
        name: "single_field",
        collName: "single_field",
        indexName: "a_1",
        keyPattern: {a: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1},
            {_id: 2, a: 1},
            {_id: 3, a: 1},
            {_id: 4, a: 1},
        ],
        ops: [
            {type: "insert", doc: {_id: 10, a: 1}, arrayPath: null, description: "scalar insert _id=10"},
            {type: "update", id: 1, setSpec: {a: 1}, arrayPath: null, description: "scalar update _id=1"},
            // First array write to "a": a single insert makes the index multikey.
            {
                type: "insert",
                doc: {_id: 11, a: [1, 2]},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "insert _id=11 arrayPath=a",
            },
            {type: "update", id: 2, setSpec: {a: [3, 4]}, arrayPath: "a", description: "update _id=2 arrayPath=a"},
            // Repeated update with the same value is a no-op (no oplog entry produced).
            {
                type: "update",
                id: 2,
                setSpec: {a: [3, 4]},
                arrayPath: "a",
                description: "duplicate update _id=2 arrayPath=a",
            },
            {
                type: "insertMany",
                docs: [
                    {_id: 12, a: 1},
                    {_id: 13, a: [5, 6]},
                    {_id: 14, a: 1},
                ],
                arrayPath: "a",
                description: "insertMany _ids=[12,14] arrayPath=a",
            },
            {
                type: "updateMany",
                ids: [3, 4],
                setSpec: {a: [7, 8]},
                arrayPath: "a",
                description: "updateMany _ids=[3,4] arrayPath=a",
            },
        ],
    },
    {
        name: "compound_sibling_paths",
        collName: "compound_sibling_paths",
        indexName: "a_1_b_1",
        keyPattern: {a: 1, b: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
            {
                indexPath: "b",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1, b: 3},
            {_id: 2, a: 1, b: 3},
            {_id: 3, a: 1, b: 3},
            {_id: 4, a: 1, b: 3},
        ],
        ops: [
            {type: "insert", doc: {_id: 10, a: 1, b: 3}, arrayPath: null, description: "scalar insert _id=10"},
            // First array write to "a": the middle document of an insertMany batch makes "a" multikey.
            {
                type: "insertMany",
                docs: [
                    {_id: 11, a: 1, b: 3},
                    {_id: 12, a: [1, 2], b: 3},
                    {_id: 13, a: 1, b: 3},
                ],
                arrayPath: "a",
                isFirstMultikey: true,
                description: "insertMany _ids=[11,13] arrayPath=a",
            },
            {type: "update", id: 1, setSpec: {a: 1, b: 3}, arrayPath: null, description: "scalar update _id=1"},
            // First array write to "b": a single update makes "b" multikey.
            {
                type: "update",
                id: 2,
                setSpec: {a: 1, b: [3, 4]},
                arrayPath: "b",
                isFirstMultikey: true,
                description: "update _id=2 arrayPath=b",
            },
            // Repeated update with the same value is a no-op (no oplog entry produced).
            {
                type: "update",
                id: 2,
                setSpec: {a: 1, b: [3, 4]},
                arrayPath: "b",
                description: "duplicate update _id=2 arrayPath=b",
            },
            {
                type: "updateMany",
                ids: [3, 4],
                setSpec: {a: [5, 6], b: 3},
                arrayPath: "a",
                description: "updateMany _ids=[3,4] arrayPath=a",
            },
        ],
    },
    {
        name: "compound_nested_paths",
        collName: "compound_nested_paths",
        indexName: "a_x_1_b_x_1",
        keyPattern: {"a.x": 1, "b.x": 1},
        fields: [
            {
                indexPath: "a.x",
                absentPath: kAbsentTwoComponentMultikeyPath,
                multikeyPath: kTwoComponentPathComponentZero,
            },
            {
                indexPath: "b.x",
                absentPath: kAbsentTwoComponentMultikeyPath,
                multikeyPath: kTwoComponentPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: {x: 1}, b: {x: 3}},
            {_id: 2, a: {x: 1}, b: {x: 3}},
            {_id: 3, a: {x: 1}, b: {x: 3}},
            {_id: 4, a: {x: 1}, b: {x: 3}},
        ],
        ops: [
            {
                type: "insert",
                doc: {_id: 10, a: {x: 1}, b: {x: 3}},
                arrayPath: null,
                description: "scalar insert _id=10",
            },
            // First array write to "a.x": an updateMany makes "a.x" multikey at its first oplog entry.
            {
                type: "updateMany",
                ids: [1, 2],
                setSpec: {a: [{x: 1}, {x: 2}], b: {x: 3}},
                arrayPath: "a.x",
                isFirstMultikey: true,
                description: "updateMany _ids=[1,2] arrayPath=a.x",
            },
            // First array write to "b.x": the middle document of an insertMany batch makes "b.x" multikey.
            {
                type: "insertMany",
                docs: [
                    {_id: 11, a: {x: 1}, b: {x: 3}},
                    {_id: 12, a: {x: 1}, b: [{x: 3}, {x: 4}]},
                    {_id: 13, a: {x: 1}, b: {x: 3}},
                ],
                arrayPath: "b.x",
                isFirstMultikey: true,
                description: "insertMany _ids=[11,13] arrayPath=b.x",
            },
            {
                type: "update",
                id: 3,
                setSpec: {a: [{x: 5}, {x: 6}], b: {x: 3}},
                arrayPath: "a.x",
                description: "update _id=3 arrayPath=a.x",
            },
            // Repeated update with the same value is a no-op (no oplog entry produced).
            {
                type: "update",
                id: 3,
                setSpec: {a: [{x: 5}, {x: 6}], b: {x: 3}},
                arrayPath: "a.x",
                description: "duplicate update _id=3 arrayPath=a.x",
            },
        ],
    },
    {
        // The first array write to "a" is an update, immediately repeated with the same value. The
        // repeat is a no-op (no oplog entry), so the transition pins to the first update.
        name: "update_first_then_noop",
        collName: "update_first_then_noop",
        indexName: "a_1",
        keyPattern: {a: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1},
            {_id: 2, a: 1},
            {_id: 3, a: 1},
        ],
        ops: [
            {type: "insert", doc: {_id: 10, a: 2}, arrayPath: null, description: "scalar insert _id=10"},
            {
                type: "update",
                id: 1,
                setSpec: {a: [1, 2]},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "update _id=1 arrayPath=a (first multikey)",
            },
            {type: "update", id: 1, setSpec: {a: [1, 2]}, arrayPath: "a", description: "duplicate no-op update _id=1"},
            {type: "update", id: 2, setSpec: {a: 3}, arrayPath: null, description: "scalar update _id=2"},
        ],
    },
    {
        // Every op stays scalar until the final op, which is the sole array write and therefore the
        // first multikey transition.
        name: "last_op_multikey",
        collName: "last_op_multikey",
        indexName: "a_1",
        keyPattern: {a: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1},
            {_id: 2, a: 1},
            {_id: 3, a: 1},
        ],
        ops: [
            {type: "insert", doc: {_id: 10, a: 2}, arrayPath: null, description: "scalar insert _id=10"},
            {type: "update", id: 1, setSpec: {a: 3}, arrayPath: null, description: "scalar update _id=1"},
            {
                type: "insertMany",
                docs: [
                    {_id: 11, a: 4},
                    {_id: 12, a: 5},
                ],
                arrayPath: null,
                description: "scalar insertMany",
            },
            {type: "updateMany", ids: [2, 3], setSpec: {a: 6}, arrayPath: null, description: "scalar updateMany"},
            {
                type: "update",
                id: 1,
                setSpec: {a: [1, 2]},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "update _id=1 arrayPath=a (last op, first multikey)",
            },
        ],
    },
    {
        // The very first op in the log is the array write, so the transition's "before" snapshot
        // resolves to the seed/barrier predecessor.
        name: "first_op_multikey",
        collName: "first_op_multikey",
        indexName: "a_1",
        keyPattern: {a: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1},
            {_id: 2, a: 1},
            {_id: 3, a: 1},
        ],
        ops: [
            {
                type: "update",
                id: 1,
                setSpec: {a: [1, 2]},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "update _id=1 arrayPath=a (first op, first multikey)",
            },
            {type: "insert", doc: {_id: 10, a: 2}, arrayPath: null, description: "scalar insert _id=10"},
            {type: "update", id: 1, setSpec: {a: [1, 2]}, arrayPath: "a", description: "duplicate no-op update _id=1"},
        ],
    },
    {
        // After the first array write, later array writes (different values, different op types) must
        // not move the recorded transition.
        name: "repeated_array_writes",
        collName: "repeated_array_writes",
        indexName: "a_1",
        keyPattern: {a: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1},
            {_id: 2, a: 1},
            {_id: 3, a: 1},
        ],
        ops: [
            {type: "insert", doc: {_id: 10, a: 2}, arrayPath: null, description: "scalar insert _id=10"},
            {
                type: "update",
                id: 1,
                setSpec: {a: [1, 2]},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "update _id=1 arrayPath=a (first multikey)",
            },
            {
                type: "update",
                id: 1,
                setSpec: {a: [3, 4]},
                arrayPath: "a",
                description: "later array update _id=1 (not first)",
            },
            {
                type: "insert",
                doc: {_id: 11, a: [5, 6]},
                arrayPath: "a",
                description: "later array insert _id=11 (not first)",
            },
        ],
    },
    {
        // Compound index where "a" first becomes multikey via a single update and "b" first becomes
        // multikey via the final insertMany batch (its middle document holds the array).
        name: "compound_update_then_insertmany",
        collName: "compound_update_then_insertmany",
        indexName: "a_1_b_1",
        keyPattern: {a: 1, b: 1},
        fields: [
            {
                indexPath: "a",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
            {
                indexPath: "b",
                absentPath: kAbsentMultikeyPath,
                multikeyPath: kPathComponentZero,
            },
        ],
        seedDocs: [
            {_id: 1, a: 1, b: 3},
            {_id: 2, a: 1, b: 3},
            {_id: 3, a: 1, b: 3},
        ],
        ops: [
            {type: "insert", doc: {_id: 10, a: 2, b: 5}, arrayPath: null, description: "scalar insert _id=10"},
            {
                type: "update",
                id: 1,
                setSpec: {a: [1, 2], b: 3},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "update _id=1 arrayPath=a (first multikey a)",
            },
            {
                type: "update",
                id: 1,
                setSpec: {a: [1, 2], b: 3},
                arrayPath: "a",
                description: "duplicate no-op update _id=1",
            },
            {
                type: "insertMany",
                docs: [
                    {_id: 11, a: 1, b: 3},
                    {_id: 12, a: 1, b: [3, 4]},
                    {_id: 13, a: 1, b: 3},
                ],
                arrayPath: "b",
                isFirstMultikey: true,
                description: "insertMany _ids=[11,13] arrayPath=b (first multikey b, last op)",
            },
        ],
    },
];

// Each wildcard case carries literal seed documents and a static operation log of wildcardInsert and
// wildcardUpdate entries. Wildcard correctness is validated by querying the indexed path at the write
// timestamp (new array element findable) and just before it (absent), rather than by reading
// catalog multikeyPaths. Each query path declares its expected values:
//   - wildcardUpdate: {path, beforeValue, afterValue} (a scalar value becomes an array element).
//   - wildcardInsert: {path, afterValue} (a new document with an array element appears).
const kWildcardCases = [
    {
        name: "all_paths",
        collName: "wildcard_all_paths",
        indexSpec: {"$**": 1},
        indexOptions: {},
        seedDocs: [
            {_id: 1, tenant: {m0: 100, m1: 101}},
            {_id: 2, tenant: {m0: 200, m1: 201}},
        ],
        ops: [
            {
                type: "wildcardUpdate",
                id: 1,
                setSpec: {"tenant.m0": [1000, 1001]},
                queryHintsForTimestampRetrieval: [{path: "tenant.m0", beforeValue: 100, afterValue: 1000}],
                isFirstMultikey: true,
                description: "wildcard update _id=1 path=tenant.m0",
            },
            {
                type: "wildcardUpdate",
                id: 2,
                setSpec: {"tenant.m1": [2010, 2011]},
                queryHintsForTimestampRetrieval: [{path: "tenant.m1", beforeValue: 201, afterValue: 2010}],
                isFirstMultikey: true,
                description: "wildcard update _id=2 path=tenant.m1",
            },
            // Repeated update with the same value is a no-op (no oplog entry produced).
            {
                type: "wildcardUpdate",
                id: 2,
                setSpec: {"tenant.m1": [2010, 2011]},
                queryHintsForTimestampRetrieval: [{path: "tenant.m1", beforeValue: 201, afterValue: 2010}],
                description: "duplicate wildcard update _id=2 path=tenant.m1",
            },
            {
                type: "wildcardInsert",
                doc: {_id: 3, tenant: {m0: [3000, 3001]}},
                queryHintsForTimestampRetrieval: [{path: "tenant.m0", afterValue: 3000}],
                isFirstMultikey: true,
                description: "wildcard insert _id=3 path=tenant.m0",
            },
        ],
    },
    {
        name: "subtree_path",
        collName: "wildcard_subtree_path",
        indexSpec: {"payload.$**": 1},
        indexOptions: {},
        seedDocs: [{_id: 1, payload: {tenant: {m0: 100}}}],
        ops: [
            {
                type: "wildcardUpdate",
                id: 1,
                setSpec: {"payload.tenant.m0": [1000, 1001]},
                queryHintsForTimestampRetrieval: [{path: "payload.tenant.m0", beforeValue: 100, afterValue: 1000}],
                isFirstMultikey: true,
                description: "wildcard update _id=1 path=payload.tenant.m0",
            },
            {
                type: "wildcardInsert",
                doc: {_id: 2, payload: {tenant: {m0: [2000, 2001]}}},
                queryHintsForTimestampRetrieval: [{path: "payload.tenant.m0", afterValue: 2000}],
                isFirstMultikey: true,
                description: "wildcard insert _id=2 path=payload.tenant.m0",
            },
        ],
    },
    {
        name: "compound_with_projection",
        collName: "wildcard_compound_with_projection",
        indexSpec: {"$**": 1, category: 1},
        indexOptions: {wildcardProjection: {category: 0}},
        seedDocs: [{_id: 1, category: "c1", tenant: {m0: 100}}],
        ops: [
            {
                type: "wildcardUpdate",
                id: 1,
                setSpec: {"tenant.m0": [1000, 1001]},
                queryHintsForTimestampRetrieval: [{path: "tenant.m0", beforeValue: 100, afterValue: 1000}],
                isFirstMultikey: true,
                description: "wildcard update _id=1 path=tenant.m0",
            },
            {
                type: "wildcardInsert",
                doc: {_id: 2, category: "c2", tenant: {m0: [2000, 2001]}},
                queryHintsForTimestampRetrieval: [{path: "tenant.m0", afterValue: 2000}],
                isFirstMultikey: true,
                description: "wildcard insert _id=2 path=tenant.m0",
            },
        ],
    },
];

////////////////////////////////////////////////////////////////////////////////
// Small helpers.
////////////////////////////////////////////////////////////////////////////////

function getLatestOplogTimestamp(node) {
    const latestOplogEntry = node.getDB("local").oplog.rs.find().sort({$natural: -1}).limit(1).next();
    return latestOplogEntry.ts;
}

/**
 * Snapshot-reads $listCatalog for one collection at `ts` and returns that index's multikeyPaths.
 *
 * Example for index {a: 1} on docs with scalar a:
 *   getIndexMultikeyPaths(node, db, "coll", "a_1", beforeTs) -> { a: BinData(0, "AA==") } // bytes: [0]
 *
 * Example after { a: [1, 2] } makes the index multikey:
 *   getIndexMultikeyPaths(node, db, "coll", "a_1", ts) -> { a: BinData(0, "AQ==") } // bytes: [1]
 */
function getIndexMultikeyPaths(node, dbName, collName, indexName, ts) {
    const catalogEntries = node
        .getDB("admin")
        .aggregate([{$listCatalog: {}}, {$match: {db: dbName, name: collName}}], {
            readConcern: {level: "snapshot", atClusterTime: ts},
        })
        .toArray();
    assert.eq(1, catalogEntries.length, catalogEntries);

    const indexMetadata = catalogEntries[0].md.indexes.find((index) => index.spec.name === indexName);
    assert.neq(undefined, indexMetadata, catalogEntries[0].md.indexes);
    return indexMetadata.multikeyPaths;
}

function makeTransitionMap(btreeCase) {
    const transitions = {};
    for (const field of btreeCase.fields) {
        transitions[field.indexPath] = null;
    }
    return transitions;
}

function fieldForArrayPath(btreeCase, arrayPath) {
    if (arrayPath === null) {
        return null;
    }
    const field = btreeCase.fields.find((f) => f.indexPath === arrayPath);
    assert(field, `unknown arrayPath "${arrayPath}" for case=${btreeCase.name}`);
    return field;
}

/**
 * A multi-document insert is grouped into a single atomic applyOps oplog entry with one timestamp
 * (see insertDocumentsAtomically in write_ops_exec.cpp). Returns that entry's timestamp, asserting
 * that the batch produced exactly one applyOps entry whose inner inserts match `docs` in order.
 */
function getInsertManyApplyOpsTimestamp(node, collName, tsBefore, docs, description) {
    const ids = docs.map((doc) => doc._id);
    const entries = node
        .getDB("local")
        .oplog.rs.find({ts: {$gt: tsBefore}, op: "c", ns: "admin.$cmd", "o.applyOps.o._id": {$in: ids}})
        .sort({$natural: 1})
        .toArray();
    assert.eq(1, entries.length, {description, expectedDocs: docs, entries});

    const innerOps = entries[0].o.applyOps;
    assert.eq(docs.length, innerOps.length, {description, innerOps});
    for (let i = 0; i < docs.length; ++i) {
        assert.eq("i", innerOps[i].op, {description, i, innerOp: innerOps[i]});
        assert.eq(docs[i]._id, innerOps[i].o._id, {description, i, innerOp: innerOps[i]});
    }
    return entries[0].ts;
}

/**
 * An updateMany produces one update oplog entry per matched document, each with its own timestamp.
 * Returns those entries in oplog order, asserting their timestamps are strictly increasing.
 */
function getUpdateManyOplogEntries(node, collName, tsBefore, ids, description) {
    const entries = node
        .getDB("local")
        .oplog.rs.find({ts: {$gt: tsBefore}, op: "u", ns: `${dbName}.${collName}`, "o2._id": {$in: ids}})
        .sort({$natural: 1})
        .toArray();

    let previousTs = tsBefore;
    for (const entry of entries) {
        assert.gt(timestampCmp(entry.ts, previousTs), 0, {description, previousTs, entry});
        previousTs = entry.ts;
    }
    return entries;
}

/**
 * Returns the oplog entries this op produced, located by the op's _id so they are the exact entries
 * this op generated rather than whatever happens to be last in the oplog. `tsBefore` is only a lower
 * bound that, together with the _id filter, uniquely identifies this op's entries.
 */
function findWriteOplogEntries(node, collName, tsBefore, opType, idFilter) {
    return node
        .getDB("local")
        .oplog.rs.find(Object.assign({ts: {$gt: tsBefore}, op: opType, ns: `${dbName}.${collName}`}, idFilter))
        .sort({$natural: 1})
        .toArray();
}

/**
 * Returns the timestamp of the single oplog entry produced by a one-document write.
 */
function getSingleWriteTimestamp(node, collName, tsBefore, opType, idFilter, description) {
    const entries = findWriteOplogEntries(node, collName, tsBefore, opType, idFilter);
    assert.eq(1, entries.length, {description, entries});
    return entries[0].ts;
}

/**
 * Returns the timestamp of the oplog entry immediately preceding `ts`. Used to derive the exact
 * "before" snapshot timestamp for a transition rather than relying on the latest oplog entry.
 */
function getPrecedingOplogTimestamp(node, ts, description) {
    const entries = node
        .getDB("local")
        .oplog.rs.find({ts: {$lt: ts}})
        .sort({$natural: -1})
        .limit(1)
        .toArray();
    assert.eq(1, entries.length, {description, ts});
    return entries[0].ts;
}

/**
 * Returns the exact oplog timestamp at which `op` first makes its array path multikey, located by the
 * op's _id(s):
 *   - insert/update: the single entry this op produced.
 *   - insertMany: the atomic applyOps entry that holds the whole batch.
 *   - updateMany: the first per-document update entry (all matched docs get the same array value, so
 *     the earliest one is when the index becomes multikey).
 */
function firstMultikeyEntryTimestamp(primary, collName, op, tsBefore) {
    if (op.type === "insert") {
        return getSingleWriteTimestamp(primary, collName, tsBefore, "i", {"o._id": op.doc._id}, op.description);
    }
    if (op.type === "update") {
        return getSingleWriteTimestamp(primary, collName, tsBefore, "u", {"o2._id": op.id}, op.description);
    }
    if (op.type === "insertMany") {
        return getInsertManyApplyOpsTimestamp(primary, collName, tsBefore, op.docs, op.description);
    }
    if (op.type === "updateMany") {
        const entries = getUpdateManyOplogEntries(primary, collName, tsBefore, op.ids, op.description);
        assert.gt(entries.length, 0, {description: op.description, entries});
        return entries[0].ts;
    }
    assert(false, `unknown op type: ${tojson(op)}`);
}

function executeBtreeOp(primary, primaryColl, btreeCase, op, opLog, firstMultikeyTransition) {
    jsTest.log.info(`Running btree op: ${op.description}`);

    // Lower bound captured BEFORE the write so that, together with the _id filter, it uniquely
    // identifies this op's oplog entry (ts strictly greater than everything written so far).
    const tsLowerBound = getLatestOplogTimestamp(primary);

    if (op.type === "insert") {
        assert.commandWorked(primaryColl.insert(op.doc));
    } else if (op.type === "update") {
        assert.commandWorked(primaryColl.updateOne({_id: op.id}, {$set: op.setSpec}));
    } else if (op.type === "insertMany") {
        assert.commandWorked(primaryColl.insertMany(op.docs));
    } else if (op.type === "updateMany") {
        assert.commandWorked(primaryColl.updateMany({_id: {$in: op.ids}}, {$set: op.setSpec}));
    } else {
        assert(false, `unknown op type: ${tojson(op)}`);
    }

    if (!op.isFirstMultikey) {
        opLog.push({description: op.description});
        return;
    }

    const field = fieldForArrayPath(btreeCase, op.arrayPath);
    assert(field, `isFirstMultikey op without an arrayPath: ${op.description}`);
    assert.eq(
        null,
        firstMultikeyTransition[field.indexPath],
        `more than one isFirstMultikey op for path "${field.indexPath}" in case=${btreeCase.name}`,
    );

    // Pin the transition to the exact entry this op produced, and take the "before" snapshot at that
    // entry's immediate oplog predecessor.
    const ts = firstMultikeyEntryTimestamp(primary, btreeCase.collName, op, tsLowerBound);
    const beforeTs = getPrecedingOplogTimestamp(primary, ts, op.description);
    const transition = {beforeTs, ts, description: op.description};
    firstMultikeyTransition[field.indexPath] = transition;
    opLog.push(transition);
}

function stopSecondaryReplicationAfterBarrier(primary, secondary, label) {
    const stopReplProducerFailPoint = configureFailPoint(secondary, "stopReplProducer");

    // Force a write to ensure the oplog fetcher is not idle and will observe stopReplProducer immediately.
    // In case there is nothing to replicate, the fetcher would wait 30s before discovering the failpoint.
    // Intentionally use {w:1} or we would block on the secondary hitting the failpoint.
    assert.commandWorked(primary.getDB(dbName).replication_barrier.insert({_id: label}, {writeConcern: {w: 1}}));
    stopReplProducerFailPoint.wait();
}

function formatMultikeyTimestampMismatch(field, snapshotTs, transition, expectedPath) {
    const expectedEnabledAt = tojson(transition.ts);
    const snapshotAt = tojson(snapshotTs);
    if (expectedPath === field.absentPath) {
        return (
            `path "${field.indexPath}": expected multikey enabled at ${expectedEnabledAt}, ` +
            `found multikey already at snapshot ${snapshotAt}`
        );
    }
    if (expectedPath === field.multikeyPath) {
        return (
            `path "${field.indexPath}": expected multikey enabled at ${expectedEnabledAt}, ` +
            `not multikey at snapshot ${snapshotAt}`
        );
    }
    return (
        `path "${field.indexPath}": expected multikey enabled at ${expectedEnabledAt}, ` +
        `multikeyPaths=${tojson(expectedPath)} at snapshot ${snapshotAt}`
    );
}

function assertPathAtTimestampForNode(node, btreeCase, field, ts, expectedPath, transition, failureContext) {
    const actual = getIndexMultikeyPaths(node, dbName, btreeCase.collName, btreeCase.indexName, ts);
    const actualPath = actual[field.indexPath];
    assert(
        tojson(expectedPath) === tojson(actualPath),
        `${formatMultikeyTimestampMismatch(field, ts, transition, expectedPath)}; ${failureContext()}`,
    );
}

function assertWildcardCollectionValid(node, collName) {
    const coll = node.getDB(dbName).getCollection(collName);
    const validateResult = assert.commandWorked(coll.validate({full: true}));
    assert(validateResult.valid, validateResult);
}

function findWithWildcardHintAtTimestamp(node, collName, path, value, ts) {
    const result = assert.commandWorked(
        node.getDB(dbName).runCommand({
            find: collName,
            filter: {[path]: value},
            hint: "wildcard_all",
            readConcern: {level: "snapshot", atClusterTime: ts},
        }),
    );
    return result.cursor.firstBatch;
}

/**
 * Executes a single wildcard op, mirroring executeBtreeOp. Returns {op, beforeTs, ts} pinned to the
 * exact oplog entry this op produced, or null when the op was a no-op (a repeated update with the
 * same value produces no oplog entry).
 */
function executeWildcardOp(primary, primaryColl, wildcardCase, op) {
    jsTest.log.info(`Running wildcard op: ${op.description}`);

    // Lower bound captured BEFORE the write so that, together with the _id filter, it uniquely
    // identifies this op's oplog entry.
    const tsLowerBound = getLatestOplogTimestamp(primary);

    let opType;
    let idFilter;
    if (op.type === "wildcardUpdate") {
        assert.commandWorked(primaryColl.updateOne({_id: op.id}, {$set: op.setSpec}));
        opType = "u";
        idFilter = {"o2._id": op.id};
    } else if (op.type === "wildcardInsert") {
        assert.commandWorked(primaryColl.insert(op.doc));
        opType = "i";
        idFilter = {"o._id": op.doc._id};
    } else {
        assert(false, `unknown wildcard op type: ${tojson(op)}`);
    }

    const entries = findWriteOplogEntries(primary, wildcardCase.collName, tsLowerBound, opType, idFilter);
    assert.lte(entries.length, 1, {description: op.description, entries});
    if (entries.length === 0) {
        return null;
    }

    const ts = entries[0].ts;
    const beforeTs = getPrecedingOplogTimestamp(primary, ts, op.description);
    return {op, beforeTs, ts};
}

/**
 * Verifies a single wildcard operation's query paths around its write timestamp:
 *   - At `ts`, the new array element value is findable on this document and the old scalar value
 *     (for updates) is not.
 *   - At `beforeTs`, the new array element value is not findable and the old scalar value (for
 *     updates) still is.
 */
function assertWildcardOpAtTimestamps(node, collName, op, beforeTs, ts) {
    const id = op.type === "wildcardInsert" ? op.doc._id : op.id;
    for (const {path, beforeValue, afterValue} of op.queryHintsForTimestampRetrieval) {
        const atTsNew = findWithWildcardHintAtTimestamp(node, collName, path, afterValue, ts);
        assert.eq(1, atTsNew.length, {host: node.host, path, afterValue, ts});
        assert.eq(id, atTsNew[0]._id, {host: node.host, path, afterValue, ts});

        const beforeNew = findWithWildcardHintAtTimestamp(node, collName, path, afterValue, beforeTs);
        assert.eq(0, beforeNew.length, {host: node.host, path, afterValue, beforeTs});

        if (beforeValue !== undefined) {
            const beforeOld = findWithWildcardHintAtTimestamp(node, collName, path, beforeValue, beforeTs);
            assert.eq(1, beforeOld.length, {host: node.host, path, beforeValue, beforeTs});
            assert.eq(id, beforeOld[0]._id, {host: node.host, path, beforeValue, beforeTs});

            const atTsOld = findWithWildcardHintAtTimestamp(node, collName, path, beforeValue, ts);
            assert.eq(0, atTsOld.length, {host: node.host, path, beforeValue, ts});
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// Test execution.
////////////////////////////////////////////////////////////////////////////////

function makeReplSet() {
    const rst = new ReplSetTest({
        nodes: [
            {},
            {
                rsConfig: {
                    priority: 0,
                    votes: 0,
                },
            },
        ],
        nodeOptions: {
            setParameter: {
                replWriterThreadCount: 8,
                replWriterMinThreadCount: 8,
            },
        },
    });
    rst.startSet();
    rst.initiate();

    assert.commandWorked(
        rst.getPrimary().adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        }),
    );

    return rst;
}

/**
 * Replays one btree case's static operation log while secondary oplog fetching is paused, then
 * restarts replication and verifies that both nodes expose the same first multikey timestamp for
 * every indexed path. The expected transition timestamp for each path comes from the operation entry
 * marked `isFirstMultikey`.
 */
function runBtreeCase(btreeCase, rst, primary, secondary) {
    jsTestLog(`Running multikey static log btree case=${btreeCase.name}`);

    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(btreeCase.collName);

    assert.commandWorked(primaryDB.dropDatabase());
    assert.commandWorked(primaryColl.createIndex(btreeCase.keyPattern, {name: btreeCase.indexName}));
    assert.commandWorked(primaryColl.insert(btreeCase.seedDocs, {writeConcern: {w: 2}}));
    rst.awaitReplication();

    stopSecondaryReplicationAfterBarrier(primary, secondary, btreeCase.name);

    const opLog = [];
    const firstMultikeyTransition = makeTransitionMap(btreeCase);

    for (const op of btreeCase.ops) {
        executeBtreeOp(primary, primaryColl, btreeCase, op, opLog, firstMultikeyTransition);
    }

    for (const field of btreeCase.fields) {
        assert.neq(
            null,
            firstMultikeyTransition[field.indexPath],
            `case=${btreeCase.name} static log has no isFirstMultikey op for path "${field.indexPath}"`,
        );
    }

    restartServerReplication(secondary);
    rst.awaitReplication();

    const failureContext = () =>
        `case=${btreeCase.name}, opLog=${tojson(opLog)}, firstMultikeyTransition=${tojson(firstMultikeyTransition)}`;

    // Check the catalog only around the first multikey transition per path (beforeTs absent, ts present).
    // Later array writes on the same path do not change when the index first became multikey.
    for (const node of [primary, secondary]) {
        for (const field of btreeCase.fields) {
            const transition = firstMultikeyTransition[field.indexPath];
            assertPathAtTimestampForNode(
                node,
                btreeCase,
                field,
                transition.beforeTs,
                field.absentPath,
                transition,
                failureContext,
            );
            assertPathAtTimestampForNode(
                node,
                btreeCase,
                field,
                transition.ts,
                field.multikeyPath,
                transition,
                failureContext,
            );
        }
    }
}

/**
 * Replays one wildcard case's static operation log while secondary oplog fetching is paused, then
 * restarts replication and verifies that the timestamped multikey metadata keys are queryable at the
 * write timestamp and not before, on both nodes. This covers the deferred metadata-key path, which
 * differs from btree multikeyPaths because wildcard multikey metadata is stored as index keys rather
 * than catalog path-level state.
 */
function runWildcardCase(wildcardCase, rst, primary, secondary) {
    jsTestLog(`Running multikey static log wildcard case=${wildcardCase.name}`);

    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(wildcardCase.collName);

    assert.commandWorked(primaryDB.dropDatabase());
    assert.commandWorked(
        primaryColl.createIndex(wildcardCase.indexSpec, {name: "wildcard_all", ...wildcardCase.indexOptions}),
    );
    assert.commandWorked(primaryColl.insert(wildcardCase.seedDocs, {writeConcern: {w: 2}}));
    rst.awaitReplication();

    stopSecondaryReplicationAfterBarrier(primary, secondary, wildcardCase.name);

    const executed = [];
    for (const op of wildcardCase.ops) {
        const result = executeWildcardOp(primary, primaryColl, wildcardCase, op);
        // A repeated no-op update produces no oplog entry; there is no transition to check.
        if (result !== null) {
            executed.push(result);
        }
    }

    restartServerReplication(secondary);
    rst.awaitReplication();

    for (const node of [primary, secondary]) {
        assertWildcardCollectionValid(node, wildcardCase.collName);
        for (const {op, beforeTs, ts} of executed) {
            assertWildcardOpAtTimestamps(node, wildcardCase.collName, op, beforeTs, ts);
        }
    }
}

const rst = makeReplSet();
try {
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    for (const btreeCase of kBtreeCases) {
        runBtreeCase(btreeCase, rst, primary, secondary);
    }
    for (const wildcardCase of kWildcardCases) {
        runWildcardCase(wildcardCase, rst, primary, secondary);
    }
} finally {
    rst.stopSet();
}
