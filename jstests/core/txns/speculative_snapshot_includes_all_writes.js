/**
 * A speculative snapshot must not include any writes ordered after any uncommitted writes.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    load("jstests/libs/check_log.js");

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

    const sessionOptions = {causalConsistency: false};
    const session = db.getMongo().startSession(sessionOptions);
    const sessionDb = session.getDatabase(dbName);
    const sessionColl = sessionDb.getCollection(collName);
    const sessionColl2 = sessionDb.getCollection(collName2);

    const session2 = db.getMongo().startSession(sessionOptions);
    const session2Db = session2.getDatabase(dbName);
    const session2Coll = session2Db.getCollection(collName);
    const session2Coll2 = session2Db.getCollection(collName2);

    // Clear ramlog so checkLog can't find log messages from previous times this fail point was
    // enabled.
    assert.commandWorked(testDB.adminCommand({clearLog: 'global'}));

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

    checkLog.contains(
        db.getMongo(),
        "hangAfterCollectionInserts fail point enabled for " + testColl2.getFullName());

    jsTest.log("Create a write following the uncommitted write.");
    // Note this write must use local write concern; it cannot be majority committed until
    // the prior uncommitted write is committed.
    assert.commandWorked(testColl.insert([{_id: 1}]));

    jsTestLog("Start a snapshot transaction.");

    session.startTransaction({readConcern: {level: "snapshot"}});

    assert.sameMembers([{_id: 0}], sessionColl.find().toArray());

    assert.sameMembers([{_id: "a"}], sessionColl2.find().toArray());

    jsTestLog("Start a majority-read transaction.");

    session2.startTransaction({readConcern: {level: "majority"}});

    assert.sameMembers([{_id: 0}, {_id: 1}], session2Coll.find().toArray());

    assert.sameMembers([{_id: "a"}], session2Coll2.find().toArray());

    jsTestLog("Allow the uncommitted write to finish.");
    assert.commandWorked(db.adminCommand({
        configureFailPoint: "hangAfterCollectionInserts",
        mode: "off",
    }));

    joinHungWrite();

    jsTestLog("Double-checking that writes not committed at start of snapshot cannot appear.");
    assert.sameMembers([{_id: 0}], sessionColl.find().toArray());

    assert.sameMembers([{_id: "a"}], sessionColl2.find().toArray());

    assert.sameMembers([{_id: 0}, {_id: 1}], session2Coll.find().toArray());

    assert.sameMembers([{_id: "a"}], session2Coll2.find().toArray());

    jsTestLog("Committing transactions.");
    session.commitTransaction();
    session2.commitTransaction();

    assert.sameMembers([{_id: 0}, {_id: 1}], sessionColl.find().toArray());

    assert.sameMembers([{_id: "a"}, {_id: "b"}], sessionColl2.find().toArray());

    assert.sameMembers([{_id: 0}, {_id: 1}], session2Coll.find().toArray());

    assert.sameMembers([{_id: "a"}, {_id: "b"}], session2Coll2.find().toArray());

    session.endSession();
}());
