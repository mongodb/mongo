/**
 * Mixed-workload test that interleaves no-txn scalar inserts, no-txn multikey updates, and
 * prepared or unprepared transaction writes on the same indexed collection while secondary oplog
 * application is paused. It then resumes replication with multi-worker apply and asserts that both
 * nodes expose the same timestamped catalog state around each path's first multikey transition.
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

const kAbsentMultikeyPath = BinData(0, "AA=="); // bytes: [0] -> {}
const kPathComponentZero = BinData(0, "AQ=="); // bytes: [1] -> {0}

const dbName = jsTestName();

// While replication is paused, each case replays an ordered phase list against the same indexed
// collection so secondary catch-up may apply no-txn writes and prepared applyOps in the same
// applier batch. Phases:
//   - preparedTxn*/unpreparedTxn*: transaction writes
//   - ScalarInsert: non-transactional scalar insert
//   - MultikeyUpdate: non-transactional update-to-array
const kMixedTxnBtreeCases = [
    {
        name: "prepared_txn_then_no_txn_scalar_insert_then_multikey_update_same_field",
        collName: "mixed_prepared_then_no_txn_scalar_insert_then_multikey_update",
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
        ],
        phases: [
            {
                type: "preparedTxnScalarInsertAndMultikeyUpdate",
                prepare: true,
                writes(sessionColl) {
                    assert.commandWorked(sessionColl.insert({_id: 10, a: 1}));
                    assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {a: [1, 2]}}));
                },
            },
            {
                type: "ScalarInsert",
                doc: {_id: 20, a: 1},
                description: "no-txn scalar insert _id=20 after prepared txn",
            },
            {
                type: "MultikeyUpdate",
                id: 20,
                setSpec: {a: [3, 4]},
                arrayPath: "a",
                description: "no-txn multikey update _id=20 field=a after prepared txn",
            },
        ],
    },
    {
        name: "no_txn_scalar_insert_prepared_txn_then_scalar_insert_then_multikey_update",
        collName: "mixed_no_txn_scalar_insert_prepared_scalar_insert_multikey_update",
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
        ],
        phases: [
            {
                type: "ScalarInsert",
                doc: {_id: 15, a: 1},
                description: "no-txn scalar insert before prepared txn",
            },
            {
                type: "preparedTxnMultikeyUpdate",
                prepare: true,
                writes(sessionColl) {
                    assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {a: [1, 2]}}));
                },
            },
            {
                type: "ScalarInsert",
                doc: {_id: 20, a: 1},
                description: "no-txn scalar insert _id=20 after prepared txn",
            },
            {
                type: "MultikeyUpdate",
                id: 20,
                setSpec: {a: [3, 4]},
                arrayPath: "a",
                description: "no-txn multikey update _id=20 field=a after prepared txn",
            },
        ],
    },
    {
        name: "no_txn_scalar_insert_then_multikey_update_then_prepared_already_multikey",
        collName: "mixed_direct_then_prepared_same_path",
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
        ],
        phases: [
            {
                type: "ScalarInsert",
                doc: {_id: 20, a: 1},
                description: "no-txn scalar insert _id=20",
            },
            {
                type: "MultikeyUpdate",
                id: 20,
                setSpec: {a: [3, 4]},
                arrayPath: "a",
                isFirstMultikey: true,
                description: "no-txn multikey update _id=20 field=a (first multikey)",
            },
            {
                type: "preparedTxnMultikeyInsert",
                prepare: true,
                writes(sessionColl) {
                    assert.commandWorked(sessionColl.insert({_id: 11, a: [5, 6]}));
                },
            },
        ],
    },
    {
        name: "prepared_txn_then_no_txn_scalar_insert_then_multikey_update_split_fields",
        collName: "mixed_prepared_then_no_txn_scalar_insert_then_multikey_update_split",
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
        ],
        phases: [
            {
                type: "preparedTxnMultikeyUpdate",
                prepare: true,
                writes(sessionColl) {
                    assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {a: [1, 2]}}));
                },
            },
            {
                type: "ScalarInsert",
                doc: {_id: 20, a: 1, b: 3},
                description: "no-txn scalar insert _id=20 after prepared txn on field=a",
            },
            {
                type: "MultikeyUpdate",
                id: 20,
                setSpec: {b: [3, 4]},
                arrayPath: "b",
                isFirstMultikey: true,
                description: "no-txn multikey update _id=20 field=b after prepared txn on field=a",
            },
        ],
    },
    {
        name: "unprepared_txn_then_no_txn_scalar_insert_then_multikey_update_same_field",
        collName: "mixed_unprepared_then_no_txn_scalar_insert_then_multikey_update",
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
        ],
        phases: [
            {
                type: "unpreparedTxnScalarInsertAndMultikeyUpdate",
                prepare: false,
                writes(sessionColl) {
                    assert.commandWorked(sessionColl.insert({_id: 10, a: 1}));
                    assert.commandWorked(sessionColl.updateOne({_id: 1}, {$set: {a: [1, 2]}}));
                },
            },
            {
                type: "ScalarInsert",
                doc: {_id: 20, a: 1},
                description: "no-txn scalar insert _id=20 after unprepared txn",
            },
            {
                type: "MultikeyUpdate",
                id: 20,
                setSpec: {a: [3, 4]},
                arrayPath: "a",
                description: "no-txn multikey update _id=20 field=a after unprepared txn",
            },
        ],
    },
];

function getLatestOplogTimestamp(node) {
    const latestOplogEntry = node
        .getDB("local")
        .oplog.rs.find()
        .sort({$natural: -1})
        .limit(1)
        .next();
    return latestOplogEntry.ts;
}

function getIndexMultikeyPaths(node, collName, indexName, ts) {
    const catalogEntries = node
        .getDB("admin")
        .aggregate([{$listCatalog: {}}, {$match: {db: dbName, name: collName}}], {
            readConcern: {level: "snapshot", atClusterTime: ts},
        })
        .toArray();
    assert.eq(1, catalogEntries.length, catalogEntries);

    const indexMetadata = catalogEntries[0].md.indexes.find(
        (index) => index.spec.name === indexName,
    );
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

function findWriteOplogEntries(node, collName, tsBefore, opType, idFilter) {
    return node
        .getDB("local")
        .oplog.rs.find(
            Object.assign({ts: {$gt: tsBefore}, op: opType, ns: `${dbName}.${collName}`}, idFilter),
        )
        .sort({$natural: 1})
        .toArray();
}

function getSingleWriteTimestamp(node, collName, tsBefore, opType, idFilter, description) {
    const entries = findWriteOplogEntries(node, collName, tsBefore, opType, idFilter);
    assert.eq(1, entries.length, {description, entries});
    return entries[0].ts;
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

function findSetMultikeyMetadataEntries(primary, collName, indexName) {
    const collFullName = `${dbName}.${collName}`;
    return primary
        .getDB("local")
        .oplog.rs.find({
            op: "c",
            ns: `${dbName}.$cmd`,
            "o.setMultikeyMetadata": collFullName,
            "o.idxName": indexName,
        })
        .sort({$natural: 1})
        .toArray();
}

function findSetMultikeyMetadataTimestampForField(primary, mixedCase, field) {
    const entries = findSetMultikeyMetadataEntries(
        primary,
        mixedCase.collName,
        mixedCase.indexName,
    );
    const matches = entries.filter(
        (entry) => tojson(entry.o.paths[field.indexPath]) === tojson(field.multikeyPath),
    );
    assert.eq(1, matches.length, {mixedCase: mixedCase.name, field, entries});
    return matches[0].ts;
}

function executeScalarInsertPhase(primary, primaryColl, mixedCase, phase) {
    jsTest.log.info(`Running no-txn scalar insert phase: ${phase.description}`);
    const tsBefore = getLatestOplogTimestamp(primary);
    assert.commandWorked(primaryColl.insert(phase.doc));
    return getSingleWriteTimestamp(
        primary,
        mixedCase.collName,
        tsBefore,
        "i",
        {"o._id": phase.doc._id},
        phase.description,
    );
}

/**
 * Mirrors the update portion of updatescalarMkDirect from multikey_timestamp_consistency: update a
 * scalar field to an array. Returns the update oplog timestamp and its immediate predecessor.
 */
function executeMultikeyUpdatePhase(primary, primaryColl, mixedCase, phase) {
    jsTest.log.info(`Running no-txn multikey update phase: ${phase.description}`);
    const tsBeforeUpdate = getLatestOplogTimestamp(primary);
    assert.commandWorked(primaryColl.updateOne({_id: phase.id}, {$set: phase.setSpec}));
    const updateTs = getSingleWriteTimestamp(
        primary,
        mixedCase.collName,
        tsBeforeUpdate,
        "u",
        {"o2._id": phase.id},
        phase.description,
    );
    const beforeUpdateTs = getPrecedingOplogTimestamp(primary, updateTs, phase.description);

    return {
        field: phase.arrayPath,
        updateTs,
        beforeUpdateTs,
        description: phase.description,
        isFirstMultikey: phase.isFirstMultikey === true,
    };
}

function runTxnWrites(primary, collName, txnSpec) {
    const session = primary.startSession({causalConsistency: false});
    const sessionColl = session.getDatabase(dbName).getCollection(collName);
    try {
        withAbortAndRetryOnTransientTxnError(session, () => {
            session.startTransaction();
            txnSpec.writes(sessionColl);
            if (txnSpec.prepare) {
                const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
                assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
            } else {
                assert.commandWorked(session.commitTransaction_forTesting());
            }
        });
    } finally {
        session.endSession();
    }
}

function buildMixedCaseTransitions(primary, mixedCase, firstMultikeyTransition) {
    const transitions = {};
    for (const field of mixedCase.fields) {
        if (firstMultikeyTransition[field.indexPath]) {
            transitions[field.indexPath] = firstMultikeyTransition[field.indexPath];
            continue;
        }
        const ts = findSetMultikeyMetadataTimestampForField(primary, mixedCase, field);
        transitions[field.indexPath] = {
            beforeTs: getPrecedingOplogTimestamp(primary, ts, mixedCase.name),
            ts,
        };
    }
    return transitions;
}

function pathIsMultikey(field, actualPath) {
    return tojson(actualPath) === tojson(field.multikeyPath);
}

function assertMultikeyUpdateAgreementAtTimestamps(
    primary,
    secondary,
    mixedCase,
    field,
    updateTs,
    beforeUpdateTs,
    failureContext,
) {
    for (const ts of [updateTs, beforeUpdateTs]) {
        const primaryPath = getIndexMultikeyPaths(
            primary,
            mixedCase.collName,
            mixedCase.indexName,
            ts,
        )[field.indexPath];
        const secondaryPath = getIndexMultikeyPaths(
            secondary,
            mixedCase.collName,
            mixedCase.indexName,
            ts,
        )[field.indexPath];
        const primaryMk = pathIsMultikey(field, primaryPath);
        const secondaryMk = pathIsMultikey(field, secondaryPath);
        assert.eq(
            primaryMk,
            secondaryMk,
            `Index multikey flag mismatch at ts ${tojson(ts)} for ${mixedCase.collName} ` +
                `field ${field.indexPath}; primary=${tojson(primaryPath)}, ` +
                `secondary=${tojson(secondaryPath)}; ${failureContext()}`,
        );
        assert.eq(
            tojson(primaryPath),
            tojson(secondaryPath),
            `scalar multikeyPaths mismatch at ts ${tojson(ts)} for ${mixedCase.collName} ` +
                `field ${field.indexPath}; ${failureContext()}`,
        );
    }
}

function stopSecondaryReplicationAfterBarrier(primary, secondary, label) {
    const stopReplProducerFailPoint = configureFailPoint(secondary, "stopReplProducer");

    // Force a write to ensure the oplog fetcher is not idle and will observe stopReplProducer immediately.
    // In case there is nothing to replicate, the fetcher would wait 30s before discovering the failpoint.
    // Intentionally use {w:1} or we would block on the secondary hitting the failpoint.
    assert.commandWorked(
        primary.getDB(dbName).replication_barrier.insert({_id: label}, {writeConcern: {w: 1}}),
    );
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

function assertPathAtTimestampForNode(
    node,
    mixedCase,
    field,
    ts,
    expectedPath,
    transition,
    failureContext,
) {
    const actual = getIndexMultikeyPaths(node, mixedCase.collName, mixedCase.indexName, ts);
    const actualPath = actual[field.indexPath];
    assert(
        tojson(expectedPath) === tojson(actualPath),
        `${formatMultikeyTimestampMismatch(field, ts, transition, expectedPath)}; ${failureContext()}`,
    );
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

    assert.commandWorked(
        rst.getPrimary().adminCommand({
            setDefaultRWConcern: 1,
            defaultWriteConcern: {w: 1},
            writeConcern: {w: "majority"},
        }),
    );

    return rst;
}

function runMixedTxnBtreeCase(mixedCase, rst, primary, secondary) {
    jsTestLog(`Running mixed txn btree case=${mixedCase.name}`);

    const primaryDB = primary.getDB(dbName);
    const primaryColl = primaryDB.getCollection(mixedCase.collName);

    assert.commandWorked(primaryDB.dropDatabase());
    assert.commandWorked(
        primaryColl.createIndex(mixedCase.keyPattern, {name: mixedCase.indexName}),
    );
    assert.commandWorked(primaryColl.insert(mixedCase.seedDocs, {writeConcern: {w: 2}}));
    rst.awaitReplication();

    stopSecondaryReplicationAfterBarrier(primary, secondary, mixedCase.name);

    const opLog = [];
    const firstMultikeyTransition = makeTransitionMap(mixedCase);
    const MultikeyUpdateVerifications = [];

    for (const phase of mixedCase.phases) {
        if (phase.type === "ScalarInsert") {
            const insertTs = executeScalarInsertPhase(primary, primaryColl, mixedCase, phase);
            opLog.push({type: "ScalarInsert", description: phase.description, ts: insertTs});
        } else if (phase.type.includes("Txn")) {
            runTxnWrites(primary, mixedCase.collName, phase);
            opLog.push({type: phase.type, prepare: phase.prepare});
        } else if (phase.type === "MultikeyUpdate") {
            const result = executeMultikeyUpdatePhase(primary, primaryColl, mixedCase, phase);
            opLog.push({type: "MultikeyUpdate", ...result});

            if (result.isFirstMultikey) {
                const field = fieldForArrayPath(mixedCase, result.field);
                assert(field, `MultikeyUpdate without field config: ${phase.description}`);
                assert.eq(
                    null,
                    firstMultikeyTransition[field.indexPath],
                    `more than one isFirstMultikey for path "${field.indexPath}" in case=${mixedCase.name}`,
                );
                firstMultikeyTransition[field.indexPath] = {
                    beforeTs: result.beforeUpdateTs,
                    ts: result.updateTs,
                    description: result.description,
                };
            }

            MultikeyUpdateVerifications.push(result);
        } else {
            assert(false, `unknown mixed phase type: ${tojson(phase)}`);
        }
    }

    restartServerReplication(secondary);
    rst.awaitReplication();

    const transitions = buildMixedCaseTransitions(primary, mixedCase, firstMultikeyTransition);
    const failureContext = () =>
        `mixedCase=${mixedCase.name}, opLog=${tojson(opLog)}, transitions=${tojson(transitions)}, ` +
        `setMultikeyMetadata=${tojson(findSetMultikeyMetadataEntries(primary, mixedCase.collName, mixedCase.indexName))}`;

    for (const node of [primary, secondary]) {
        for (const field of mixedCase.fields) {
            const transition = transitions[field.indexPath];
            if (!transition) {
                continue;
            }

            assertPathAtTimestampForNode(
                node,
                mixedCase,
                field,
                transition.beforeTs,
                field.absentPath,
                transition,
                failureContext,
            );
            assertPathAtTimestampForNode(
                node,
                mixedCase,
                field,
                transition.ts,
                field.multikeyPath,
                transition,
                failureContext,
            );
        }
    }

    for (const MultikeyUpdate of MultikeyUpdateVerifications) {
        const field = fieldForArrayPath(mixedCase, MultikeyUpdate.field);
        assert(
            field,
            `MultikeyUpdate verification without field config: ${MultikeyUpdate.description}`,
        );
        assertMultikeyUpdateAgreementAtTimestamps(
            primary,
            secondary,
            mixedCase,
            field,
            MultikeyUpdate.updateTs,
            MultikeyUpdate.beforeUpdateTs,
            failureContext,
        );
    }
}

const rst = makeReplSet();
try {
    const primary = rst.getPrimary();
    const secondary = rst.getSecondary();

    for (const mixedCase of kMixedTxnBtreeCases) {
        runMixedTxnBtreeCase(mixedCase, rst, primary, secondary);
    }
} finally {
    rst.stopSet();
}
