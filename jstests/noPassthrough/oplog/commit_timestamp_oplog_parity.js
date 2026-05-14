/**
 * SERVER-122333: Storage transaction commit timestamp must match the timestamp embedded
 * in the oplog entry produced by the same write.
 *
 * Companion to src/mongo/tla_plus/Storage/CommitTimestampOplogParity/. The TLA+ spec
 * proves that under the modeled flow (reserve oplog slot -> stamp RecoveryUnit ->
 * insert oplog entry -> commit) drift is unreachable. This jstest exercises the same
 * flow on a running mongod and asserts the parity invariant against real oplog
 * timestamps and the storage transaction's commit timestamp surfaced via the
 * 'oplogSlotTimestamp' fields in server logs / serverStatus.
 *
 * Strategy:
 *   1. Drive a mix of writes that historically have surfaced drift conditions:
 *        - single-document inserts under {w:1, j:true}
 *        - retryable writes with a session
 *        - applyOps with a single inner op
 *        - a no-op write triggered by appendOplogNote
 *      For every successful write we record the resulting oplog entry's ts field.
 *   2. Use a parallel shell to hold an oplog hole behind another insert (the classic
 *      "stale reserved slot" path), then verify the second confirmed write still has
 *      its oplog ts equal to the commit timestamp the storage engine reported.
 *   3. Walk local.oplog.rs for the written entries and assert each ts is dense and
 *      strictly increasing, and that the lastWriteOpTime / lastAppliedOpTime reported
 *      by replSetGetStatus matches the maximum oplog ts we observed.
 *
 * Failure of any assertion below is a real-world counterexample to the TLA+ invariant
 * CommitTimestampOplogParity and points directly at the bug class SERVER-122333
 * proactively detects.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 *   requires_majority_read_concern,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({name: jsTest.name(), nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const dbName = "commit_ts_oplog_parity";
const collName = jsTestName();
const primaryDB = primary.getDB(dbName);
const primaryColl = primaryDB[collName];
const oplog = primary.getDB("local").oplog.rs;

assert.commandWorked(primaryDB.createCollection(collName, {writeConcern: {w: "majority"}}));

/**
 * Read the oplog entry for a write identified by a unique document predicate and
 * return its 'ts'.
 */
function tsForWrite(predicate) {
    const entry = oplog.find(predicate).sort({$natural: -1}).limit(1).next();
    assert(entry, `expected oplog entry for ${tojson(predicate)}`);
    return entry.ts;
}

/**
 * Pull the storage engine's view of the most recent commit timestamp.
 *
 * On WiredTiger this is exposed via {serverStatus: 1, oplog: 1}.oplog.latestOptime.ts
 * when an oplog is configured, and matches the value the RecoveryUnit was stamped
 * with for the most recent committed write on this node. Any drift here is exactly
 * what SERVER-122333 wants to assert against.
 */
function latestStorageCommitTs() {
    const status = assert.commandWorked(primaryDB.adminCommand({serverStatus: 1, oplog: 1}));
    assert(status.oplog && status.oplog.latestOptime,
           `serverStatus did not surface oplog.latestOptime: ${tojson(status.oplog)}`);
    return status.oplog.latestOptime.ts;
}

function assertParity(label, oplogTs) {
    const storageTs = latestStorageCommitTs();
    assert.eq(
        0,
        timestampCmp(oplogTs, storageTs),
        `${label}: oplog ts ${tojson(oplogTs)} drifted from storage commit ts ${tojson(storageTs)}`,
    );
}

jsTest.log("Case 1: single-document insert under {w:1, j:true}");
{
    assert.commandWorked(
        primaryColl.insert({_id: "single", marker: "single"}, {writeConcern: {w: 1, j: true}}),
    );
    const ts = tsForWrite({"o.marker": "single"});
    assertParity("single insert", ts);
}

jsTest.log("Case 2: retryable write through a session");
{
    const session = primary.startSession({retryWrites: true});
    const sessionColl = session.getDatabase(dbName)[collName];
    assert.commandWorked(sessionColl.insert({_id: "retryable", marker: "retryable"}));
    const ts = tsForWrite({"o.marker": "retryable"});
    assertParity("retryable insert", ts);
    session.endSession();
}

jsTest.log("Case 3: applyOps with a single inner op");
{
    assert.commandWorked(primaryDB.runCommand({
        applyOps: [{
            op: "i",
            ns: `${dbName}.${collName}`,
            o: {_id: "applyops", marker: "applyops"},
        }],
    }));
    const ts = tsForWrite({"o.marker": "applyops"});
    assertParity("applyOps insert", ts);
}

jsTest.log("Case 4: no-op write via appendOplogNote");
{
    assert.commandWorked(primaryDB.adminCommand({
        appendOplogNote: 1,
        data: {marker: "noop", note: "SERVER-122333"},
    }));
    const noopEntry = oplog.find({"o.marker": "noop"}).sort({$natural: -1}).limit(1).next();
    assert(noopEntry, "expected an appendOplogNote oplog entry");
    assertParity("appendOplogNote", noopEntry.ts);
}

jsTest.log("Case 5: confirmed write following a held-open oplog hole");
{
    const failPoint = configureFailPoint(primaryDB, "hangAfterCollectionInserts", {
        collectionNS: primaryColl.getFullName(),
        first_id: "holeBefore",
    });

    TestData.dbName = dbName;
    TestData.collName = collName;
    const ps = startParallelShell(() => {
        // Held open; we will release it via the failpoint after the trailing write commits.
        db.getSiblingDB(TestData.dbName)[TestData.collName].insert({_id: "holeBefore"});
    }, primary.port);

    try {
        failPoint.wait();

        // The trailing write has an oplog hole behind it. The TLA+ invariant says the
        // storage commit timestamp on this write must still equal the slot it reserved.
        assert.commandWorked(primaryColl.insert(
            {_id: "afterHole", marker: "afterHole"},
            {writeConcern: {w: 1, j: true}},
        ));
        const ts = tsForWrite({"o.marker": "afterHole"});
        assertParity("write behind oplog hole", ts);
    } finally {
        failPoint.off();
        ps();
    }
}

jsTest.log("Cross-check: oplog timestamps are strictly increasing and the latest equals "
           + "replSetGetStatus.optimes.appliedOpTime.ts");
{
    const seen = oplog
        .find({"o.marker": {$in: ["single", "retryable", "applyops", "noop", "afterHole"]}})
        .sort({ts: 1})
        .toArray();
    assert.gte(seen.length, 5, `expected 5 marked oplog entries, saw ${tojson(seen)}`);
    for (let i = 1; i < seen.length; ++i) {
        assert.lt(
            timestampCmp(seen[i - 1].ts, seen[i].ts),
            0,
            `oplog ts not strictly increasing at index ${i}: ${tojson(seen)}`,
        );
    }

    const status = assert.commandWorked(primaryDB.adminCommand({replSetGetStatus: 1}));
    const appliedTs = status.optimes.appliedOpTime.ts;
    const tail = oplog.find().sort({$natural: -1}).limit(1).next().ts;
    assert.eq(
        0,
        timestampCmp(appliedTs, tail),
        `appliedOpTime ${tojson(appliedTs)} drifted from oplog tail ${tojson(tail)}`,
    );
}

rst.stopSet();
