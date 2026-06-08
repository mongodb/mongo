/**
 * Verifies that 'setMultikeyMetadata' oplog entries emitted by multi-document transactions are
 * applied during standalone replication recovery with 'recoverToOplogTimestamp' in read-only mode
 * (queryableBackupMode). This apply path is distinct from magic restore.
 *
 * Each case makes an index multikey inside a transaction, recovers a standalone node past that
 * transaction, and asserts the index is multikey.
 *
 * @tags: [
 *   featureFlagReplicateMultikeynessInTransactions,
 *   requires_majority_read_concern,
 *   requires_persistence,
 *   requires_replication,
 *   uses_transactions,
 * ]
 */

import {getPlanStage, getWinningPlanFromExplain} from "jstests/libs/query/analyze_plan.js";
import {afterEach, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const dbName = "test";

let rst;
let rstStoppedForRestart = false;
let standalone;

// Builds a collection with 'keyPattern', makes it multikey inside a transaction, then recovers a
// standalone node past that transaction. Returns the read-only recovered database.
function recoverWithMultikeyTxn({collName, keyPattern, indexName, indexOptions = {}, initialDoc, multikeyUpdate}) {
    rst = new ReplSetTest({nodes: 1});
    rst.startSet();
    rst.initiate();
    rstStoppedForRestart = false;

    const primary = rst.getPrimary();
    const primaryDB = primary.getDB(dbName);
    const coll = primaryDB.getCollection(collName);

    // The collection and a non-multikey index live in the checkpoint; recovery only replays the
    // multikey transition.
    assert.commandWorked(coll.insert(initialDoc));
    assert.commandWorked(coll.createIndex(keyPattern, {name: indexName, ...indexOptions}));

    // Pin the stable timestamp so the transaction below stays in the oplog for recovery to replay.
    const recoveryTimestamp = assert.commandWorked(primaryDB.runCommand({ping: 1})).operationTime;
    assert.commandWorked(
        primaryDB.adminCommand({
            configureFailPoint: "holdStableTimestampAtSpecificTimestamp",
            mode: "alwaysOn",
            data: {timestamp: recoveryTimestamp},
        }),
    );

    // Make the index multikey inside a multi-document transaction; this emits setMultikeyMetadata.
    const session = primary.startSession();
    const sessionColl = session.getDatabase(dbName).getCollection(collName);
    session.startTransaction();
    assert.commandWorked(sessionColl.update(multikeyUpdate.q, multikeyUpdate.u));
    const operationTime = assert.commandWorked(session.commitTransaction_forTesting()).operationTime;

    // One setMultikeyMetadata entry is emitted per affected index, regardless of path count.
    const cmdNs = primaryDB.getCollection("$cmd").getFullName();
    const entries = rst.dumpOplog(primary, {op: "c", ns: cmdNs, "o.setMultikeyMetadata": coll.getFullName()});
    assert.eq(1, entries.length, "expected one setMultikeyMetadata oplog entry", {entries});

    const dbpath = primary.dbpath;
    rst.stopSet(/*signal=*/ null, /*forRestart=*/ true);
    rstStoppedForRestart = true;

    // Recover a standalone up to and including the multikey transaction.
    standalone = MongoRunner.runMongod({
        dbpath: dbpath,
        noReplSet: true,
        noCleanData: true,
        queryableBackupMode: "",
        setParameter: {recoverToOplogTimestamp: tojson({timestamp: operationTime})},
    });
    return standalone.getDB(dbName);
}

// Asserts that 'indexName' is multikey by hinting it and inspecting the IXSCAN stage.
function assertMultikey(db, collName, indexName, pathQuery) {
    const explain = db.getCollection(collName).find(pathQuery).hint(indexName).explain();
    const ixscan = getPlanStage(getWinningPlanFromExplain(explain), "IXSCAN");
    assert.eq(ixscan.isMultiKey, true, "expected index to be multikey after recovery", {indexName, ixscan});
}

describe("setMultikeyMetadata during standalone recoverToOplogTimestamp", function () {
    afterEach(function () {
        if (standalone) {
            MongoRunner.stopMongod(standalone);
            standalone = null;
        }
        // On the happy path the helper already stopped the set for restart; only tear down here if a
        // case failed before reaching that point.
        if (rst && !rstStoppedForRestart) {
            rst.stopSet();
        }
        rst = null;
    });

    it("marks a regular index multikey", function () {
        const db = recoverWithMultikeyTxn({
            collName: "regular",
            keyPattern: {a: 1},
            indexName: "a_1",
            initialDoc: {_id: 1, a: 1},
            multikeyUpdate: {q: {_id: 1}, u: {$set: {a: [1, 2, 3]}}},
        });
        assert.eq(db.getCollection("regular").findOne({_id: 1}), {_id: 1, a: [1, 2, 3]});
        assertMultikey(db, "regular", "a_1", {a: 1});
    });

    it("marks a wildcard index multikey", function () {
        const db = recoverWithMultikeyTxn({
            collName: "wildcard",
            keyPattern: {"$**": 1},
            indexName: "wildcard_1",
            initialDoc: {_id: 1, a: 1},
            multikeyUpdate: {q: {_id: 1}, u: {$set: {a: [1, 2, 3]}}},
        });
        assertMultikey(db, "wildcard", "wildcard_1", {a: 1});
    });

    it("regenerates multikey metadata for a compound wildcard index", function () {
        const db = recoverWithMultikeyTxn({
            collName: "compoundWildcard",
            keyPattern: {x: 1, "$**": 1},
            indexName: "compound_wildcard",
            // A compound wildcard index over '$**' must scope the wildcard away from the other keys.
            indexOptions: {wildcardProjection: {x: 0}},
            initialDoc: {_id: 1, x: 1, a: 1},
            multikeyUpdate: {q: {_id: 1}, u: {$set: {a: [1, 2, 3]}}},
        });
        assertMultikey(db, "compoundWildcard", "compound_wildcard", {x: 1, a: 1});
    });

    it("marks multiple wildcard paths multikey from a single transaction", function () {
        const db = recoverWithMultikeyTxn({
            collName: "multiPath",
            keyPattern: {"$**": 1},
            indexName: "wildcard_1",
            initialDoc: {_id: 1, a: 1, b: 1},
            multikeyUpdate: {q: {_id: 1}, u: {$set: {a: [1, 2], b: [3, 4]}}},
        });
        assertMultikey(db, "multiPath", "wildcard_1", {a: 1});
        assertMultikey(db, "multiPath", "wildcard_1", {b: 3});
    });
});
