/**
 * Test basic retryable write without errors by checking that the resulting collection after the
 * retry is as expected and it does not create additional oplog entries.
 */
(function() {

    "use strict";

    var checkFindAndModifyResult = function(expected, toCheck) {
        assert.eq(expected.ok, toCheck.ok);
        assert.eq(expected.value, toCheck.value);

        // TODO: SERVER-30532: after adding upserted, just compare the entire lastErrorObject
        var expectedLE = expected.lastErrorObject;
        var toCheckLE = toCheck.lastErrorObject;

        assert.neq(null, toCheckLE);
        assert.eq(expected.updatedExisting, toCheck.updatedExisting);
        assert.eq(expected.n, toCheck.n);
    };

    var runTests = function(mainConn, priConn) {
        var lsid = UUID();

        ////////////////////////////////////////////////////////////////////////
        // Test insert command

        var cmd = {
            insert: 'user',
            documents: [{_id: 10}, {_id: 30}],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(34),
        };

        var testDBMain = mainConn.getDB('test');
        var result = assert.commandWorked(testDBMain.runCommand(cmd));

        var oplog = priConn.getDB('local').oplog.rs;
        var insertOplogEntries = oplog.find({ns: 'test.user', op: 'i'}).itcount();

        var testDBPri = priConn.getDB('test');
        assert.eq(2, testDBPri.user.find().itcount());

        var retryResult = assert.commandWorked(testDBMain.runCommand(cmd));
        assert.eq(result.ok, retryResult.ok);
        assert.eq(result.n, retryResult.n);
        assert.eq(result.writeErrors, retryResult.writeErrors);
        assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

        assert.eq(2, testDBPri.user.find().itcount());
        assert.eq(insertOplogEntries, oplog.find({ns: 'test.user', op: 'i'}).itcount());

        ////////////////////////////////////////////////////////////////////////
        // Test update command

        cmd = {
            update: 'user',
            updates: [
                {q: {_id: 10}, u: {$inc: {x: 1}}},  // in place
                {q: {_id: 20}, u: {$inc: {y: 1}}, upsert: true},
                {q: {_id: 30}, u: {z: 1}}  // replacement
            ],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(35),
        };

        result = assert.commandWorked(testDBMain.runCommand(cmd));

        let updateOplogEntries = oplog.find({ns: 'test.user', op: 'u'}).itcount();

        // Upserts are stored as inserts in the oplog, so check inserts too.
        insertOplogEntries = oplog.find({ns: 'test.user', op: 'i'}).itcount();

        assert.eq(3, testDBPri.user.find().itcount());

        retryResult = assert.commandWorked(testDBMain.runCommand(cmd));
        assert.eq(result.ok, retryResult.ok);
        assert.eq(result.n, retryResult.n);
        assert.eq(result.nModified, retryResult.nModified);
        assert.eq(result.upserted, retryResult.upserted);
        assert.eq(result.writeErrors, retryResult.writeErrors);
        assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

        assert.eq(3, testDBPri.user.find().itcount());

        assert.eq({_id: 10, x: 1}, testDBPri.user.findOne({_id: 10}));
        assert.eq({_id: 20, y: 1}, testDBPri.user.findOne({_id: 20}));
        assert.eq({_id: 30, z: 1}, testDBPri.user.findOne({_id: 30}));

        assert.eq(updateOplogEntries, oplog.find({ns: 'test.user', op: 'u'}).itcount());
        assert.eq(insertOplogEntries, oplog.find({ns: 'test.user', op: 'i'}).itcount());

        ////////////////////////////////////////////////////////////////////////
        // Test delete command

        assert.writeOK(testDBMain.user.insert({_id: 40, x: 1}));
        assert.writeOK(testDBMain.user.insert({_id: 50, y: 1}));

        assert.eq(2, testDBPri.user.find({x: 1}).itcount());
        assert.eq(2, testDBPri.user.find({y: 1}).itcount());

        cmd = {
            delete: 'user',
            deletes: [{q: {x: 1}, limit: 1}, {q: {y: 1}, limit: 1}],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(36),
        };

        result = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        let deleteOplogEntries = oplog.find({ns: 'test.user', op: 'd'}).itcount();

        assert.eq(1, testDBPri.user.find({x: 1}).itcount());
        assert.eq(1, testDBPri.user.find({y: 1}).itcount());

        retryResult = assert.commandWorked(testDBMain.runCommand(cmd));
        assert.eq(result.ok, retryResult.ok);
        assert.eq(result.n, retryResult.n);
        assert.eq(result.writeErrors, retryResult.writeErrors);
        assert.eq(result.writeConcernErrors, retryResult.writeConcernErrors);

        assert.eq(1, testDBPri.user.find({x: 1}).itcount());
        assert.eq(1, testDBPri.user.find({y: 1}).itcount());

        assert.eq(deleteOplogEntries, oplog.find({ns: 'test.user', op: 'd'}).itcount());

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (upsert)

        cmd = {
            findAndModify: 'user',
            query: {_id: 60},
            update: {$inc: {x: 1}},
            new: true,
            upsert: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(37),
        };

        result = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
        insertOplogEntries = oplog.find({ns: 'test.user', op: 'i'}).itcount();
        updateOplogEntries = oplog.find({ns: 'test.user', op: 'u'}).itcount();
        assert.eq({_id: 60, x: 1}, testDBPri.user.findOne({_id: 60}));

        retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

        assert.eq({_id: 60, x: 1}, testDBPri.user.findOne({_id: 60}));
        assert.eq(insertOplogEntries, oplog.find({ns: 'test.user', op: 'i'}).itcount());
        assert.eq(updateOplogEntries, oplog.find({ns: 'test.user', op: 'u'}).itcount());

        checkFindAndModifyResult(result, retryResult);

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (update, return pre-image)

        cmd = {
            findAndModify: 'user',
            query: {_id: 60},
            update: {$inc: {x: 1}},
            new: false,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(38),
        };

        result = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
        var oplogEntries = oplog.find({ns: 'test.user', op: 'u'}).itcount();
        assert.eq({_id: 60, x: 2}, testDBPri.user.findOne({_id: 60}));

        retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

        assert.eq({_id: 60, x: 2}, testDBPri.user.findOne({_id: 60}));
        assert.eq(oplogEntries, oplog.find({ns: 'test.user', op: 'u'}).itcount());

        checkFindAndModifyResult(result, retryResult);

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (update, return post-image)

        cmd = {
            findAndModify: 'user',
            query: {_id: 60},
            update: {$inc: {x: 1}},
            new: true,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(39),
        };

        result = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
        oplogEntries = oplog.find({ns: 'test.user', op: 'u'}).itcount();
        assert.eq({_id: 60, x: 3}, testDBPri.user.findOne({_id: 60}));

        retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

        assert.eq({_id: 60, x: 3}, testDBPri.user.findOne({_id: 60}));
        assert.eq(oplogEntries, oplog.find({ns: 'test.user', op: 'u'}).itcount());

        checkFindAndModifyResult(result, retryResult);

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (remove, return pre-image)

        assert.writeOK(testDBMain.user.insert({_id: 70, f: 1}));
        assert.writeOK(testDBMain.user.insert({_id: 80, f: 1}));

        cmd = {
            findAndModify: 'user',
            query: {f: 1},
            remove: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(40),
        };

        result = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
        oplogEntries = oplog.find({ns: 'test.user', op: 'd'}).itcount();
        var docCount = testDBPri.user.find().itcount();

        retryResult = assert.commandWorked(testDBMain.runCommand(cmd));

        assert.eq(oplogEntries, oplog.find({ns: 'test.user', op: 'd'}).itcount());
        assert.eq(docCount, testDBPri.user.find().itcount());

        checkFindAndModifyResult(result, retryResult);
    };

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    var priConn = replTest.getPrimary();

    runTests(priConn, priConn);

    replTest.stopSet();

    var st = new ShardingTest({shards: {rs0: {nodes: 1}}});

    runTests(st.s, st.rs0.getPrimary());

    st.stop();
})();
