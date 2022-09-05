/**
 * A speculative snapshot must not include any writes ordered after any uncommitted writes.
 *
 * @tags: [
 *  uses_transactions,
 *  requires_majority_read_concern,
 *  uses_parallel_shell,
 *  # 'setDefaultRWConcern' is not supposed to be run on shard nodes.
 *  command_not_supported_in_serverless,
 * ]
 */
(function() {
"use strict";

const dbName = "test";
const collName = "speculative_snapshot_includes_all_writes_1";
const collName2 = "speculative_snapshot_includes_all_writes_2";
const testDB = db.getSiblingDB(dbName);
const testColl = testDB[collName];
const testColl2 = testDB[collName2];

testDB.runCommand({drop: collName, writeConcern: {w: "majority"}});
testDB.runCommand({drop: collName2, writeConcern: {w: "majority"}});

assert.commandWorked(testDB.createCollection(collName, {writeConcern: {w: "majority"}}));
assert.commandWorked(testDB.createCollection(collName2, {writeConcern: {w: "majority"}}));

const sessionOptions = {
    causalConsistency: false
};

function startSessionAndTransaction(readConcernLevel) {
    let session = db.getMongo().startSession(sessionOptions);
    jsTestLog("Start a transaction with readConcern " + readConcernLevel.level + ".");
    session.startTransaction({readConcern: readConcernLevel});
    return session;
}

let checkReads = (session, collExpected, coll2Expected) => {
    let sessionDb = session.getDatabase(dbName);
    let coll = sessionDb.getCollection(collName);
    let coll2 = sessionDb.getCollection(collName2);
    assert.sameMembers(collExpected, coll.find().toArray());
    assert.sameMembers(coll2Expected, coll2.find().toArray());
};

// Clear ramlog so checkLog can't find log messages from previous times this fail point was
// enabled.
assert.commandWorked(testDB.adminCommand({clearLog: 'global'}));

try {
    // The default WC is majority and this test can't satisfy majority writes.
    assert.commandWorked(testDB.adminCommand(
        {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

    jsTest.log("Prepopulate the collections.");
    assert.commandWorked(testColl.insert([{_id: 0}], {writeConcern: {w: "majority"}}));
    assert.commandWorked(testColl2.insert([{_id: "a"}], {writeConcern: {w: "majority"}}));

    jsTest.log("Create the uncommitted write.");

    assert.commandWorked(db.adminCommand({
        configureFailPoint: "hangAfterCollectionInserts",
        mode: "alwaysOn",
        data: {collectionNS: testColl2.getFullName()}
    }));

    const joinHungWrite = startParallelShell(() => {
        assert.commandWorked(
            db.getSiblingDB("test").speculative_snapshot_includes_all_writes_2.insert(
                {_id: "b"}, {writeConcern: {w: "majority"}}));
    });

    checkLog.containsJson(db.getMongo(), 20289);

    jsTest.log("Create a write following the uncommitted write.");
    // Note this write must use local write concern; it cannot be majority committed until
    // the prior uncommitted write is committed.
    assert.commandWorked(testColl.insert([{_id: 1}]));

    const snapshotSession = startSessionAndTransaction({level: "snapshot"});
    checkReads(snapshotSession, [{_id: 0}], [{_id: "a"}]);

    const majoritySession = startSessionAndTransaction({level: "majority"});
    checkReads(majoritySession, [{_id: 0}, {_id: 1}], [{_id: "a"}]);

    const localSession = startSessionAndTransaction({level: "local"});
    checkReads(localSession, [{_id: 0}, {_id: 1}], [{_id: "a"}]);

    const defaultSession = startSessionAndTransaction({});
    checkReads(defaultSession, [{_id: 0}, {_id: 1}], [{_id: "a"}]);

    jsTestLog("Allow the uncommitted write to finish.");
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "hangAfterCollectionInserts",
        mode: "off",
    }));

    joinHungWrite();

    jsTestLog("Double-checking that writes not committed at start of snapshot cannot appear.");
    checkReads(snapshotSession, [{_id: 0}], [{_id: "a"}]);

    jsTestLog(
        "Double-checking that writes performed before the start of a transaction of 'majority' or lower must appear.");
    checkReads(majoritySession, [{_id: 0}, {_id: 1}], [{_id: "a"}]);
    checkReads(localSession, [{_id: 0}, {_id: 1}], [{_id: "a"}]);
    checkReads(defaultSession, [{_id: 0}, {_id: 1}], [{_id: "a"}]);

    jsTestLog("Committing transactions.");
    assert.commandWorked(snapshotSession.commitTransaction_forTesting());
    assert.commandWorked(majoritySession.commitTransaction_forTesting());
    assert.commandWorked(localSession.commitTransaction_forTesting());
    assert.commandWorked(defaultSession.commitTransaction_forTesting());

    jsTestLog("A new local read must see all committed writes.");
    checkReads(defaultSession, [{_id: 0}, {_id: 1}], [{_id: "a"}, {_id: "b"}]);

    snapshotSession.endSession();
    majoritySession.endSession();
    localSession.endSession();
    defaultSession.endSession();
} finally {
    // Unsetting CWWC is not allowed, so explicitly restore the default write concern to be majority
    // by setting CWWC to {w: majority}.
    assert.commandWorked(testDB.adminCommand({
        setDefaultRWConcern: 1,
        defaultWriteConcern: {w: "majority"},
        writeConcern: {w: "majority"}
    }));
}
}());
