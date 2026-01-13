/**
 * Helper to test different prepare-transaction oplog formats and verify that the config.transactions
 * table entries match between the primary and secondary.
 *
 */

import {PrepareHelpers} from "jstests/core/txns/libs/prepare_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {getOplogEntriesForTxnOnNode} from "jstests/sharding/libs/sharded_transactions_helpers.js";

// TODO (SERVER-115115): Remove this function once stopReplicaSet includes built-in consistency checks.
function checkCollectionDataConsistency(primary, secondary, dbs, colls) {
    jsTest.log.info("Checking collection data consistency between primary and secondary");

    for (let i = 0; i < dbs.length; i++) {
        const dbName = dbs[i];
        const collName = colls[i];

        const primaryColl = primary.getDB(dbName).getCollection(collName);
        const secondaryColl = secondary.getDB(dbName).getCollection(collName);

        // Check document count
        const primaryCount = primaryColl.count();
        const secondaryCount = secondaryColl.count();
        assert.eq(
            primaryCount,
            secondaryCount,
            `Document count mismatch for ${dbName}.${collName}: ` +
                `primary=${primaryCount}, secondary=${secondaryCount}`,
        );

        // Check actual documents (sorted by _id for consistent comparison)
        const primaryDocs = primaryColl.find().sort({_id: 1}).toArray();
        const secondaryDocs = secondaryColl.find().sort({_id: 1}).toArray();

        assert.eq(primaryDocs.length, secondaryDocs.length, `Document array length mismatch for ${dbName}.${collName}`);

        for (let j = 0; j < primaryDocs.length; j++) {
            assert.docEq(primaryDocs[j], secondaryDocs[j], `Document mismatch at index ${j} in ${dbName}.${collName}`);
        }

        jsTest.log.info(`✓ Collection ${dbName}.${collName} is consistent ` + `(${primaryCount} documents)`);
    }
}

export function checkPrepareTxnTableUpdate(primary, secondary, commitOrAbort, checkConsistency = false) {
    Random.setRandomSeed();
    const checkTransactionTableEntry = (lsid, txnNumber, preparedTs, expectedAffectedNamespaces) => {
        const primaryTxnEntry = primary.getDB("config").transactions.findOne({"_id.id": lsid.id, "txnNum": txnNumber});
        const secondaryTxnEntry = secondary
            .getDB("config")
            .transactions.findOne({"_id.id": lsid.id, "txnNum": txnNumber});
        const isMultiversion =
            Boolean(jsTest.options().useRandomBinVersionsWithinReplicaSet) || Boolean(TestData.multiversionBinVersion);
        if (isMultiversion) {
            delete primaryTxnEntry.affectedNamespaces;
            delete secondaryTxnEntry.affectedNamespaces;
        }

        assert.eq(primaryTxnEntry, secondaryTxnEntry);
        assert.eq(primaryTxnEntry["state"], "prepared");
        assert.eq(primaryTxnEntry["lastWriteOpTime"]["ts"], preparedTs);

        if (FeatureFlagUtil.isPresentAndEnabled(primary, "PreparedTransactionsPreciseCheckpoints") && !isMultiversion) {
            assert.eq(primaryTxnEntry["affectedNamespaces"], expectedAffectedNamespaces);
        }
    };

    const finishTransaction = (session, prepareTimestamp) => {
        jsTest.log.info("Finish transaction with: " + commitOrAbort);
        if (commitOrAbort === "commit") {
            assert.commandWorked(PrepareHelpers.commitTransaction(session, prepareTimestamp));
        } else if (commitOrAbort === "abort") {
            assert.commandWorked(session.abortTransaction_forTesting());
        } else {
            assert(false, "Unrecognized transaction decision: " + commitOrAbort);
        }
    };

    const rst = new ReplSetTest(primary.host);
    const largeId = 90009;
    const numberOfCollections = 50;
    let id = 0;
    let dbs = [];
    let colls = [];
    for (let i = 0; i < numberOfCollections; i++) {
        dbs.push(`prepare_txn_update_db${i}`);
        colls.push(`coll${i}`);
        assert(primary.getDB(dbs[i]).getCollection(colls[i]).drop());
        assert.commandWorked(primary.getDB(dbs[i]).getCollection(colls[i]).insert({_id: largeId}));
    }

    const doPrepareTest = (expectedAffectedNamespaces, opsFunc, oneOplog) => {
        const session = primary.startSession();
        session.startTransaction();
        opsFunc(session);
        const prepareTimestamp = PrepareHelpers.prepareTransaction(session);
        rst.awaitLastOpCommitted(undefined, [secondary]);
        const lsid = session._serverSession.handle.getId();
        const txnNumber = session._serverSession.handle.getTxnNumber();

        if (oneOplog) {
            assert.eq(getOplogEntriesForTxnOnNode(primary, lsid, txnNumber).length, 1);
        } else {
            assert.gt(getOplogEntriesForTxnOnNode(primary, lsid, txnNumber).length, 1);
        }
        checkTransactionTableEntry(lsid, txnNumber, prepareTimestamp, expectedAffectedNamespaces);
        finishTransaction(session, prepareTimestamp);

        if (checkConsistency) {
            // Wait for replication and check data consistency after transaction completes
            rst.awaitLastOpCommitted(undefined, [secondary]);
            checkCollectionDataConsistency(primary, secondary, dbs, colls);
        }
    };

    jsTest.log.info("Test read-only transaction, with: " + commitOrAbort);
    doPrepareTest(
        [],
        (session) => {
            let coll = session.getDatabase(dbs[0]).getCollection(colls[0]);
            coll.findOne();
        },
        true /* oneOplog */,
    );

    jsTest.log.info("Test no-op transaction, with: " + commitOrAbort);
    doPrepareTest(
        [],
        (session) => {
            let coll = session.getDatabase(dbs[0]).getCollection(colls[0]);
            assert.commandWorked(coll.deleteOne({_id: id++}));
        },
        true /* oneOplog */,
    );

    jsTest.log.info("Test transaction with one operation, with: " + commitOrAbort);
    doPrepareTest(
        [`${dbs[0]}.${colls[0]}`],
        (session) => {
            let coll = session.getDatabase(dbs[0]).getCollection(colls[0]);
            assert.commandWorked(coll.insert({_id: id++}));
        },
        true /* oneOplog */,
    );

    const expectedAffectedNamespaces = Array.from({length: numberOfCollections}, (_, i) => `${dbs[i]}.${colls[i]}`);
    expectedAffectedNamespaces.sort();
    const arr = Array.from({length: numberOfCollections}, (_, i) => i);

    jsTest.log.info("Test transaction with many operations that fits in one oplog, with: " + commitOrAbort);
    doPrepareTest(
        expectedAffectedNamespaces,
        (session) => {
            [0, 1].forEach((round) => {
                let shuffled = Array.shuffle(arr);
                shuffled.forEach((value, i) => {
                    let doWrite = round == 0 || (round == 1 && i < 25);
                    let coll = session.getDatabase(dbs[value]).getCollection(colls[value]);
                    // Some collections are written once, and others are written twice.
                    if (doWrite) {
                        if (i % 3 == 0) {
                            assert.commandWorked(coll.update({_id: largeId}, {$set: {"updated": id++}}));
                        } else if (i % 3 == 1) {
                            assert.commandWorked(coll.deleteOne({_id: largeId}));
                            assert.commandWorked(coll.insert({_id: largeId}));
                        } else {
                            assert.commandWorked(coll.insert({_id: id++}));
                        }
                    } else {
                        assert.eq(coll.find({_id: largeId}).itcount(), 1);
                    }
                });
            });
        },
        true /* oneOplog */,
    );

    jsTest.log.info(
        "Test a transaction with many operations that spans multiple oplog entries, with: " + commitOrAbort,
    );
    const kSize10MB = 10 * 1024 * 1024;
    const createLargeDocument = (id) => ({
        _id: id,
        longString: "a".repeat(kSize10MB),
    });
    doPrepareTest(
        expectedAffectedNamespaces,
        (session) => {
            [0, 1].forEach((round) => {
                let shuffled = Array.shuffle(arr);
                shuffled.forEach((value, i) => {
                    let coll = session.getDatabase(dbs[value]).getCollection(colls[value]);
                    let doWrite = round == 0 || (round == 1 && i < 25);
                    // Some collections are written once, and others are written twice.
                    if (doWrite) {
                        if (i % 3 == 0) {
                            assert.commandWorked(coll.update({_id: largeId}, {$set: {"updated": id++}}));
                        } else if (i % 3 == 1) {
                            assert.commandWorked(coll.deleteOne({_id: largeId}));
                            assert.commandWorked(coll.insert({_id: largeId}));
                        } else {
                            assert.commandWorked(coll.insert({_id: id++}));
                        }
                        if (i % 10 == 0) {
                            assert.commandWorked(coll.insert(createLargeDocument(id++)));
                        }
                    } else {
                        assert.eq(coll.find({_id: largeId}).itcount(), 1);
                    }
                });
            });
        },
        false /* oneOplog */,
    );
}
