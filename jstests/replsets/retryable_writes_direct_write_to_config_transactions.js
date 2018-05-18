// Validates the expected behaviour of direct writes against the `config.transactions` collection
(function() {
    'use strict';

    // Direct writes to config.transactions cannot be part of a session.
    TestData.disableImplicitSessions = true;

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    var replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();

    var priConn = replTest.getPrimary();
    var db = priConn.getDB('TestDB');
    var config = priConn.getDB('config');

    assert.writeOK(db.user.insert({_id: 0}));
    assert.writeOK(db.user.insert({_id: 1}));

    const lsid1 = UUID();
    const lsid2 = UUID();

    const cmdObj1 = {
        update: 'user',
        updates: [{q: {_id: 0}, u: {$inc: {x: 1}}}],
        lsid: {id: lsid1},
        txnNumber: NumberLong(1)
    };
    assert.commandWorked(db.runCommand(cmdObj1));
    assert.eq(1, db.user.find({_id: 0}).toArray()[0].x);

    const cmdObj2 = {
        update: 'user',
        updates: [{q: {_id: 1}, u: {$inc: {x: 1}}}],
        lsid: {id: lsid2},
        txnNumber: NumberLong(1)
    };
    assert.commandWorked(db.runCommand(cmdObj2));
    assert.eq(1, db.user.find({_id: 1}).toArray()[0].x);

    assert.eq(1, config.transactions.find({'_id.id': lsid1}).itcount());
    assert.eq(1, config.transactions.find({'_id.id': lsid2}).itcount());

    // Invalidating lsid1 doesn't impact lsid2, but allows same statement to be executed again
    assert.writeOK(config.transactions.remove({'_id.id': lsid1}));
    assert.commandWorked(db.runCommand(cmdObj1));
    assert.eq(2, db.user.find({_id: 0}).toArray()[0].x);
    assert.commandWorked(db.runCommand(cmdObj2));
    assert.eq(1, db.user.find({_id: 1}).toArray()[0].x);

    // Ensure lsid1 is properly tracked after the recreate
    assert.commandWorked(db.runCommand(cmdObj1));
    assert.eq(2, db.user.find({_id: 0}).toArray()[0].x);

    // Ensure garbage data cannot be written to the `config.transactions` collection
    assert.writeError(config.transactions.insert({_id: 'String'}));
    assert.writeError(config.transactions.insert({_id: {UnknownField: 'Garbage'}}));

    // Ensure inserting an invalid session record manually without all the required fields causes
    // the session to not work anymore for retryable writes for that session, but not for any other
    const lsidManual = config.transactions.find({'_id.id': lsid1}).toArray()[0]._id;
    assert.writeOK(config.transactions.remove({'_id.id': lsid1}));
    assert.writeOK(config.transactions.insert({_id: lsidManual}));

    const lsid3 = UUID();
    assert.commandWorked(db.runCommand({
        update: 'user',
        updates: [{q: {_id: 2}, u: {$inc: {x: 1}}, upsert: true}],
        lsid: {id: lsid3},
        txnNumber: NumberLong(1)
    }));
    assert.eq(1, db.user.find({_id: 2}).toArray()[0].x);

    // Ensure dropping the `config.transactions` collection breaks the retryable writes feature, but
    // doesn't crash the server
    assert(config.transactions.drop());
    var res = assert.commandWorked(db.runCommand(cmdObj2));
    assert.eq(0, res.nModified);
    assert.eq(1, db.user.find({_id: 1}).toArray()[0].x);

    assert(config.dropDatabase());
    res = assert.commandWorked(db.runCommand(cmdObj2));
    assert.eq(0, res.nModified);
    assert.eq(1, db.user.find({_id: 1}).toArray()[0].x);

    replTest.stopSet();
})();
