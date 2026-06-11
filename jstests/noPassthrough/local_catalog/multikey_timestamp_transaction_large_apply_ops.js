/**
 * Tests that multikey metadata from large transactions is timestamp-consistent between primary and
 * secondary.
 *
 * Large transactions deliberately exceed the 16MB applyOps limit so the commit is represented by an
 * applyOps chain. The test pauses secondary replication and verifies both nodes agree on when each
 * index path becomes multikey for regular and wildcard indexes, including unprepared and prepared
 * transactions against pre-created indexes.
 *
 * @tags: [
 *   featureFlagReplicateMultikeynessInTransactions,
 *   requires_persistence,
 *   requires_replication,
 *   requires_snapshot_read,
 *   uses_transactions,
 *   uses_prepare_transaction,
 * ]
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {withAbortAndRetryOnTransientTxnError} from "jstests/libs/auto_retry_transaction_in_sharding.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {restartServerReplication} from "jstests/libs/write_concern_util.js";
import {getOplogEntriesForTxnOnNode} from "jstests/sharding/libs/sharded_transactions_helpers.js";

const kAbsentMultikeyPath = BinData(0, "AA=="); // bytes: [0] -> {}
const kPathComponentZero = BinData(0, "AQ=="); // bytes: [1] -> {0}

const dbName = jsTestName();

const kLargeTxnPaddingBytes = 128 * 1024; // 128KB per doc
const kLargeTxnPadding = "x".repeat(kLargeTxnPaddingBytes);
const k16MB = 16 * 1024 * 1024;
const kLargeTxnFillerDocCount = 130;
assert.gt(kLargeTxnPadding.length * kLargeTxnFillerDocCount, k16MB);

const kCompoundTxnFields = [
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
];

const kTransactionBtreeCases = [
    {
        name: "regular_unprepared_same_txn_create",
        collName: "txn_regular_unprepared_same_txn_create",
        indexName: "a_1_b_1",
        keyPattern: {a: 1, b: 1},
        fields: kCompoundTxnFields,
        seedDocs: [],
        createIndexInsideTxn: true,
        prepare: false,
        expectedMultikeyTimestampSource: "commit",
        txnWrites(sessionColl, caseName) {
            assert.commandWorked(
                sessionColl.insert([
                    {_id: 1, a: 1, b: 3},
                    {_id: 2, a: 1, b: 3},
                ]),
            );
            assert.commandWorked(sessionColl.insert({_id: 10, a: 1, b: 3}));
            assert.commandWorked(sessionColl.insert({_id: 11, a: [1, 2], b: 3}));
            assert.commandWorked(sessionColl.updateOne({_id: 10}, {$set: {b: [3, 4]}}));
        },
    },
    {
        name: "regular_unprepared_precreated_index",
        collName: "txn_regular_unprepared_precreated_index",
        indexName: "a_1_b_1",
        keyPattern: {a: 1, b: 1},
        fields: kCompoundTxnFields,
        seedDocs: [
            {_id: 1, a: 1, b: 3},
            {_id: 2, a: 1, b: 3},
        ],
        createIndexInsideTxn: false,
        prepare: false,
        expectedMultikeyTimestampSource: "setMultikeyMetadata",
        txnWrites(sessionColl, caseName) {
            assert.commandWorked(sessionColl.insert({_id: 10, a: 1, b: 3}));
            assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {a: [1, 2]}}));
            assert.commandWorked(sessionColl.updateOne({_id: 2}, {$set: {b: [3, 4]}}));
        },
    },
    {
        name: "regular_prepared_precreated_index",
        collName: "txn_regular_prepared_precreated_index",
        indexName: "a_1_b_1",
        keyPattern: {a: 1, b: 1},
        fields: kCompoundTxnFields,
        seedDocs: [
            {_id: 1, a: 1, b: 3},
            {_id: 2, a: 1, b: 3},
        ],
        createIndexInsideTxn: false,
        prepare: true,
        expectedMultikeyTimestampSource: "setMultikeyMetadata",
        txnWrites(sessionColl, caseName) {
            assert.commandWorked(sessionColl.insert({_id: 10, a: 1, b: 3}));
            assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {a: [1, 2]}}));
            assert.commandWorked(sessionColl.updateOne({_id: 2}, {$set: {b: [3, 4]}}));
        },
    },
];

const kTransactionWildcardCases = [
    {
        name: "wildcard_unprepared_same_txn_create",
        collName: "txn_wildcard_unprepared_same_txn_create",
        indexSpec: {"$**": 1},
        indexOptions: {},
        seedDocs: [],
        createIndexInsideTxn: true,
        prepare: false,
        expectedMultikeyTimestampSource: "commit",
        queryHintsForTimestampRetrieval: [
            {path: "nested.alpha", beforeValue: 100, afterValue: 1000},
            {path: "nested.beta", beforeValue: 201, afterValue: 2010},
        ],
        txnWrites(sessionColl, caseName) {
            assert.commandWorked(sessionColl.insert({_id: 1, nested: {alpha: 100, beta: 201}}));
            assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {"nested.alpha": [1000, 1001]}}));
            assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {"nested.beta": [2010, 2011]}}));
        },
    },
    {
        name: "wildcard_prepared_precreated_index",
        collName: "txn_wildcard_prepared_precreated_index",
        indexSpec: {"$**": 1},
        indexOptions: {},
        seedDocs: [{_id: 1, nested: {alpha: 100, beta: 201}}],
        createIndexInsideTxn: false,
        prepare: true,
        expectedMultikeyTimestampSource: "setMultikeyMetadata",
        queryHintsForTimestampRetrieval: [
            {path: "nested.alpha", beforeValue: 100, afterValue: 1000},
            {path: "nested.beta", beforeValue: 201, afterValue: 2010},
        ],
        txnWrites(sessionColl, caseName) {
            assert.commandWorked(sessionColl.insert({_id: 10, nested: {alpha: 200, beta: 301}}));
            assert.commandWorked(
                sessionColl.updateOne({_id: 1}, {$set: {"nested.alpha": [1000, 1001], "nested.beta": [2010, 2011]}}),
            );
        },
    },
];

function getCatalogEntry(node, collName, ts) {
    const catalogEntries = node
        .getDB("admin")
        .aggregate([{$listCatalog: {}}, {$match: {db: dbName, name: collName}}], {
            readConcern: {level: "snapshot", atClusterTime: ts},
        })
        .toArray();
    assert.lte(catalogEntries.length, 1, {host: node.host, collName, ts, catalogEntries});
    return catalogEntries.length === 0 ? null : catalogEntries[0];
}

function getIndexMultikeyPaths(node, collName, indexName, ts) {
    const catalogEntry = getCatalogEntry(node, collName, ts);
    assert.neq(null, catalogEntry, {host: node.host, collName, indexName, ts});

    const indexMetadata = catalogEntry.md.indexes.find((index) => index.spec.name === indexName);
    assert.neq(undefined, indexMetadata, catalogEntry.md.indexes);
    return indexMetadata.multikeyPaths;
}

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

function assertPathAtTimestampForNode(node, txnCase, field, ts, expectedPath, transition, failureContext) {
    const actual = getIndexMultikeyPaths(node, txnCase.collName, txnCase.indexName, ts);
    const actualPath = actual[field.indexPath];
    assert(
        tojson(expectedPath) === tojson(actualPath),
        `${formatMultikeyTimestampMismatch(field, ts, transition, expectedPath)}; ${failureContext()}`,
    );
}

function assertCollectionMissingAtTimestamp(node, collName, ts, failureContext) {
    const catalogEntry = getCatalogEntry(node, collName, ts);
    assert.eq(null, catalogEntry, `${collName} unexpectedly exists at ${tojson(ts)}; ${failureContext()}`);
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

function findWithWildcardHintAtTimestampOrMissingCollection(node, collName, path, value, ts) {
    const result = node.getDB(dbName).runCommand({
        find: collName,
        filter: {[path]: value},
        hint: "wildcard_all",
        readConcern: {level: "snapshot", atClusterTime: ts},
    });
    if (!result.ok) {
        assert.eq(ErrorCodes.NamespaceNotFound, result.code, {host: node.host, collName, path, value, ts, result});
        return [];
    }
    return result.cursor.firstBatch;
}

function appendLargeTxnFillerWrites(sessionColl, caseName) {
    const fillerDocs = [];
    for (let i = 0; i < kLargeTxnFillerDocCount; ++i) {
        fillerDocs.push({_id: `${caseName}_filler_${i}`, padding: kLargeTxnPadding});
    }

    const batchSize = 10;
    for (let i = 0; i < fillerDocs.length; i += batchSize) {
        assert.commandWorked(sessionColl.insertMany(fillerDocs.slice(i, i + batchSize)));
    }
}

function assertTransactionOplogChainSplit(primary, session, txnCase) {
    const lsid = session._serverSession.handle.getId();
    const txnNumber = session._serverSession.handle.getTxnNumber();
    const oplogEntries = getOplogEntriesForTxnOnNode(primary, lsid, txnNumber);
    const applyOpsEntries = oplogEntries.filter((entry) => entry.o.applyOps);
    assert.gt(applyOpsEntries.length, 1, "expected large transaction to span multiple applyOps oplog entries", {
        txnCase: txnCase.name,
        oplogEntries,
    });

    for (const [index, entry] of applyOpsEntries.entries()) {
        assert.eq("c", entry.op, {txnCase: txnCase.name, entry});
        assert.eq("admin.$cmd", entry.ns, {txnCase: txnCase.name, entry});
        jsTestLog(
            `txn oplog chain case=${txnCase.name} link=${index + 1}/${applyOpsEntries.length} ` +
                `ts=${tojson(entry.ts)} innerOps=${entry.o.applyOps.length}`,
        );
    }
}

function findUnpreparedTransactionCommitTimestamp(primary, collName, txnCase) {
    const collFullName = `${dbName}.${collName}`;
    const entries = primary
        .getDB("local")
        .oplog.rs.find({op: "c", ns: "admin.$cmd", "o.applyOps.ns": collFullName})
        .sort({ts: -1})
        .toArray();
    assert.gt(entries.length, 0, {txnCase: txnCase.name, collName, entries});
    return entries[0].ts;
}

function findSetMultikeyMetadataEntries(primary, collName, indexName) {
    const collFullName = `${dbName}.${collName}`;
    const entries = primary
        .getDB("local")
        .oplog.rs.find({
            op: "c",
            ns: `${dbName}.$cmd`,
            "o.setMultikeyMetadata": collFullName,
            "o.idxName": indexName,
        })
        .sort({$natural: 1})
        .toArray();
    assert.gt(entries.length, 0, {collName, indexName, entries});
    return entries;
}

function findSetMultikeyMetadataTimestamp(primary, collName, indexName) {
    const entries = findSetMultikeyMetadataEntries(primary, collName, indexName);
    assert.eq(1, entries.length, {collName, indexName, entries});
    return entries[0].ts;
}

function findSetMultikeyMetadataTimestampForField(primary, txnCase, field) {
    const entries = findSetMultikeyMetadataEntries(primary, txnCase.collName, txnCase.indexName);
    const matches = entries.filter((entry) => tojson(entry.o.paths[field.indexPath]) === tojson(field.multikeyPath));
    assert.eq(1, matches.length, {txnCase: txnCase.name, field, entries});
    return matches[0].ts;
}

function buildBtreeTransitions(primary, txnCase) {
    const transitions = {};
    for (const field of txnCase.fields) {
        const ts =
            txnCase.expectedMultikeyTimestampSource === "commit"
                ? findUnpreparedTransactionCommitTimestamp(primary, txnCase.collName, txnCase)
                : findSetMultikeyMetadataTimestampForField(primary, txnCase, field);
        transitions[field.indexPath] = {beforeTs: getPrecedingOplogTimestamp(primary, ts, txnCase.name), ts};
    }
    return transitions;
}

function assertTransactionBtreeCase(txnCase, primary, secondary) {
    const transitions = buildBtreeTransitions(primary, txnCase);
    const failureContext = () => `txnCase=${txnCase.name}, transitions=${tojson(transitions)}`;

    for (const node of [primary, secondary]) {
        for (const field of txnCase.fields) {
            const transition = transitions[field.indexPath];
            if (txnCase.createIndexInsideTxn) {
                assertCollectionMissingAtTimestamp(node, txnCase.collName, transition.beforeTs, failureContext);
            } else {
                assertPathAtTimestampForNode(
                    node,
                    txnCase,
                    field,
                    transition.beforeTs,
                    field.absentPath,
                    transition,
                    failureContext,
                );
            }
            assertPathAtTimestampForNode(
                node,
                txnCase,
                field,
                transition.ts,
                field.multikeyPath,
                transition,
                failureContext,
            );
        }
    }
}

function assertWildcardTxnHintsAtTimestamps(node, collName, queryHints, docId, beforeTs, ts, allowMissingBefore) {
    const findBeforeTs = allowMissingBefore
        ? findWithWildcardHintAtTimestampOrMissingCollection
        : findWithWildcardHintAtTimestamp;
    for (const {path, beforeValue, afterValue} of queryHints) {
        const atTsNew = findWithWildcardHintAtTimestamp(node, collName, path, afterValue, ts);
        assert.eq(1, atTsNew.length, {host: node.host, path, afterValue, ts});
        assert.eq(docId, atTsNew[0]._id, {host: node.host, path, afterValue, ts});

        const beforeNew = findBeforeTs(node, collName, path, afterValue, beforeTs);
        assert.eq(0, beforeNew.length, {host: node.host, path, afterValue, beforeTs});

        if (beforeValue !== undefined && !allowMissingBefore) {
            const beforeOld = findWithWildcardHintAtTimestamp(node, collName, path, beforeValue, beforeTs);
            assert.eq(1, beforeOld.length, {host: node.host, path, beforeValue, beforeTs});
            assert.eq(docId, beforeOld[0]._id, {host: node.host, path, beforeValue, beforeTs});

            const atTsOld = findWithWildcardHintAtTimestamp(node, collName, path, beforeValue, ts);
            assert.eq(0, atTsOld.length, {host: node.host, path, beforeValue, ts});
        }
    }
}

function assertTransactionWildcardCase(txnCase, primary, secondary) {
    const visibilityTs = findUnpreparedTransactionCommitTimestamp(primary, txnCase.collName, txnCase);
    const beforeTs = getPrecedingOplogTimestamp(primary, visibilityTs, txnCase.name);

    for (const node of [primary, secondary]) {
        assertWildcardCollectionValid(node, txnCase.collName);
        assertWildcardTxnHintsAtTimestamps(
            node,
            txnCase.collName,
            txnCase.queryHintsForTimestampRetrieval,
            1,
            beforeTs,
            visibilityTs,
            txnCase.createIndexInsideTxn,
        );
    }
}

function runLargeTransactionCase(txnCase, rst, primary, secondary) {
    jsTestLog(`Executing test case - large transaction case=${txnCase.name}`);

    const primaryDB = primary.getDB(dbName);
    const isBtreeCase = txnCase.keyPattern !== undefined;

    assert.commandWorked(primaryDB.dropDatabase());

    if (!txnCase.createIndexInsideTxn) {
        const primaryColl = primaryDB.getCollection(txnCase.collName);
        if (isBtreeCase) {
            assert.commandWorked(primaryColl.createIndex(txnCase.keyPattern, {name: txnCase.indexName}));
        } else {
            assert.commandWorked(
                primaryColl.createIndex(txnCase.indexSpec, {name: "wildcard_all", ...txnCase.indexOptions}),
            );
        }
        assert.commandWorked(primaryColl.insert(txnCase.seedDocs, {writeConcern: {w: 2}}));
        rst.awaitReplication();
    }

    stopSecondaryReplicationAfterBarrier(primary, secondary, txnCase.name);

    const session = primary.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB.getCollection(txnCase.collName);
    try {
        withAbortAndRetryOnTransientTxnError(session, () => {
            session.startTransaction();

            if (txnCase.createIndexInsideTxn) {
                assert.commandWorked(sessionDB.createCollection(txnCase.collName));
                if (isBtreeCase) {
                    assert.commandWorked(sessionColl.createIndex(txnCase.keyPattern, {name: txnCase.indexName}));
                } else {
                    assert.commandWorked(
                        sessionColl.createIndex(txnCase.indexSpec, {name: "wildcard_all", ...txnCase.indexOptions}),
                    );
                }
            }

            txnCase.txnWrites(sessionColl, txnCase.name);
            appendLargeTxnFillerWrites(sessionColl, txnCase.name);
            assert.commandWorked(sessionColl.updateOne({_id: `${txnCase.name}_filler_0`}, {$set: {tag: "updated"}}));
            assert.commandWorked(sessionColl.deleteOne({_id: `${txnCase.name}_filler_1`}));

            if (txnCase.prepare) {
                const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
            } else {
                assert.commandWorked(session.commitTransaction_forTesting());
            }
        });

        assertTransactionOplogChainSplit(primary, session, txnCase);
    } finally {
        session.endSession();
        restartServerReplication(secondary);
    }
    rst.awaitReplication();

    if (isBtreeCase) {
        assertTransactionBtreeCase(txnCase, primary, secondary);
    } else {
        assertTransactionWildcardCase(txnCase, primary, secondary);
    }
}

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

    // The test sests a failpoint to stop temporarely replication.
    // Every operation must run with write concern {w:1} or we would
    // block on the secondary hitting the failpoint.
    assert.commandWorked(
        rst.getPrimary().adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        }),
    );

    return rst;
}

const rst = makeReplSet();
try {
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    for (const txnCase of kTransactionBtreeCases) {
        runLargeTransactionCase(txnCase, rst, primary, secondary);
    }
    for (const txnCase of kTransactionWildcardCases) {
        runLargeTransactionCase(txnCase, rst, primary, secondary);
    }
} finally {
    rst.stopSet();
}
