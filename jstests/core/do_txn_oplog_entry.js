// @tags: [requires_non_retryable_commands]

// Tests that doTxn produces correct oplog entries.
(function() {
    'use strict';
    // For isWiredTiger.
    load("jstests/concurrency/fsm_workload_helpers/server_types.js");
    // For isReplSet
    load("jstests/libs/fixture_helpers.js");
    load('jstests/libs/uuid_util.js');

    if (!isWiredTiger(db)) {
        jsTestLog("Skipping test as the storage engine does not support doTxn.");
        return;
    }
    if (!FixtureHelpers.isReplSet(db)) {
        jsTestLog("Skipping test as doTxn requires a replSet and replication is not enabled.");
        return;
    }

    var oplog = db.getSiblingDB('local').oplog.rs;
    var session = db.getMongo().startSession();
    var sessionDb = session.getDatabase("test");
    var t = db.doTxn;
    t.drop();
    db.createCollection(t.getName());
    const tUuid = getUUIDFromListCollections(db, t.getName());

    //
    // Test insert ops.  Insert ops are unmodified except the UUID field is added.
    //
    const insertOps = [
        {op: 'i', ns: t.getFullName(), o: {_id: 100, x: 1, y: 1}},
        {op: 'i', ns: t.getFullName(), o: {_id: 101, x: 2, y: 1}},
    ];
    assert.commandWorked(sessionDb.runCommand({doTxn: insertOps, txnNumber: NumberLong("1")}));
    let topOfOplog = oplog.find().sort({$natural: -1}).limit(1).next();
    assert.eq(topOfOplog.txnNumber, NumberLong("1"));
    assert.docEq(topOfOplog.o.applyOps, insertOps.map(x => Object.assign(x, {ui: tUuid})));

    //
    // Test update ops.  For updates, the "$v" UpdateSemantics field is added and non-idempotent
    // operations are made idempotent.
    //
    const updateOps = [
        {op: 'u', ns: t.getFullName(), o: {$inc: {x: 10}}, o2: {_id: 100}},
        {op: 'u', ns: t.getFullName(), o: {$inc: {x: 10}}, o2: {_id: 101}}
    ];
    const expectedUpdateOps = [
        {op: 'u', ns: t.getFullName(), o: {$v: 1, $set: {x: 11}}, o2: {_id: 100}, ui: tUuid},
        {op: 'u', ns: t.getFullName(), o: {$v: 1, $set: {x: 12}}, o2: {_id: 101}, ui: tUuid}
    ];
    assert.commandWorked(sessionDb.runCommand({doTxn: updateOps, txnNumber: NumberLong("2")}));
    topOfOplog = oplog.find().sort({$natural: -1}).limit(1).next();
    assert.eq(topOfOplog.txnNumber, NumberLong("2"));
    assert.docEq(topOfOplog.o.applyOps, expectedUpdateOps);

    //
    // Test delete ops.  Delete ops are unmodified except the UUID field is added.
    //
    const deleteOps = [
        {op: 'd', ns: t.getFullName(), o: {_id: 100}},
        {op: 'd', ns: t.getFullName(), o: {_id: 101}}
    ];
    assert.commandWorked(sessionDb.runCommand({doTxn: deleteOps, txnNumber: NumberLong("3")}));
    topOfOplog = oplog.find().sort({$natural: -1}).limit(1).next();
    assert.eq(topOfOplog.txnNumber, NumberLong("3"));
    assert.docEq(topOfOplog.o.applyOps, deleteOps.map(x => Object.assign(x, {ui: tUuid})));

    //
    // Make sure the transaction table is not affected by one-shot transactions.
    //
    assert.eq(0,
              db.getSiblingDB('config')
                  .transactions.find({"_id.id": session.getSessionId().id})
                  .toArray(),
              "No transactions should be written to the transaction table.");

    session.endSession();
})();
