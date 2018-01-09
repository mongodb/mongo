/**
 * Test that the oplog entries will contain the correct logical session id, transaction number and
 * statement id after executing a write command. Also tests that the session table is properly
 * updated after the write operations.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    var checkOplog = function(oplog, lsid, uid, txnNum, stmtId, prevTs, prevTerm) {
        assert(oplog != null);
        assert(oplog.lsid != null);
        assert.eq(lsid, oplog.lsid.id);
        assert.eq(uid, oplog.lsid.uid);
        assert.eq(txnNum, oplog.txnNumber);
        assert.eq(stmtId, oplog.stmtId);

        var oplogPrevTs = oplog.prevOpTime.ts;
        assert.eq(prevTs.getTime(), oplogPrevTs.getTime());
        assert.eq(prevTs.getInc(), oplogPrevTs.getInc());
        assert.eq(prevTerm, oplog.prevOpTime.t);
    };

    var checkSessionCatalog = function(conn, sessionId, uid, txnNum, expectedTs, expectedTerm) {
        var coll = conn.getDB('config').transactions;
        var sessionDoc = coll.findOne({'_id': {id: sessionId, uid: uid}});

        assert.eq(txnNum, sessionDoc.txnNum);

        var oplogTs = sessionDoc.lastWriteOpTime.ts;
        assert.eq(expectedTs.getTime(), oplogTs.getTime());
        assert.eq(expectedTs.getInc(), oplogTs.getInc());

        assert.eq(expectedTerm, sessionDoc.lastWriteOpTime.t);
    };

    var runTests = function(mainConn, priConn) {
        var lsid = UUID();
        var uid = function() {
            var user = mainConn.getDB("admin")
                           .runCommand({connectionStatus: 1})
                           .authInfo.authenticatedUsers[0];

            if (user) {
                return computeSHA256Block(user.user + "@" + user.db);
            } else {
                return computeSHA256Block("");
            }
        }();

        var txnNumber = NumberLong(34);
        var incrementTxnNumber = function() {
            txnNumber = NumberLong(txnNumber + 1);
        };

        ////////////////////////////////////////////////////////////////////////
        // Test insert command

        var cmd = {
            insert: 'user',
            documents: [{_id: 10}, {_id: 30}],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        var oplog = priConn.getDB('local').oplog.rs;

        var firstDoc = oplog.findOne({ns: 'test.user', 'o._id': 10});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        var secondDoc = oplog.findOne({ns: 'test.user', 'o._id': 30});
        checkOplog(secondDoc, lsid, uid, txnNumber, 1, firstDoc.ts, firstDoc.t);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, secondDoc.ts, secondDoc.t);

        ////////////////////////////////////////////////////////////////////////
        // Test update command

        incrementTxnNumber();
        cmd = {
            update: 'user',
            updates: [
                {q: {_id: 10}, u: {$set: {x: 1}}},  // in place
                {q: {_id: 20}, u: {$set: {y: 1}}, upsert: true},
                {q: {_id: 30}, u: {z: 1}}  // replacement
            ],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 10});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        secondDoc = oplog.findOne({ns: 'test.user', op: 'i', 'o._id': 20});
        checkOplog(secondDoc, lsid, uid, txnNumber, 1, firstDoc.ts, firstDoc.t);

        var thirdDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 30});
        checkOplog(thirdDoc, lsid, uid, txnNumber, 2, secondDoc.ts, secondDoc.t);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, thirdDoc.ts, thirdDoc.t);

        ////////////////////////////////////////////////////////////////////////
        // Test delete command

        incrementTxnNumber();
        cmd = {
            delete: 'user',
            deletes: [{q: {_id: 10}, limit: 1}, {q: {_id: 20}, limit: 1}],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 10});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        secondDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 20});
        checkOplog(secondDoc, lsid, uid, txnNumber, 1, firstDoc.ts, firstDoc.t);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, secondDoc.ts, secondDoc.t);

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (upsert)

        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: 40},
            update: {$set: {x: 1}},
            new: true,
            upsert: true,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'i', 'o._id': 40});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        assert.eq(null, firstDoc.preImageTs);
        assert.eq(null, firstDoc.postImageTs);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        var lastTs = firstDoc.ts;

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (in-place update, return pre-image)

        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: 40},
            update: {$inc: {x: 1}},
            new: false,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        var beforeDoc = mainConn.getDB('test').user.findOne({_id: 40});
        var res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 40, ts: {$gt: lastTs}});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        assert.eq(null, firstDoc.postImageTs);

        var savedDoc = oplog.findOne({
            ns: 'test.user',
            op: 'n',
            ts: firstDoc.preImageOpTime.ts,
            t: firstDoc.preImageOpTime.t
        });
        assert.eq(beforeDoc, savedDoc.o);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        lastTs = firstDoc.ts;

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (in-place update, return post-image)

        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: 40},
            update: {$inc: {x: 1}},
            new: true,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
        var afterDoc = mainConn.getDB('test').user.findOne({_id: 40});

        firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 40, ts: {$gt: lastTs}});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        assert.eq(null, firstDoc.preImageTs);

        savedDoc = oplog.findOne({
            ns: 'test.user',
            op: 'n',
            ts: firstDoc.postImageOpTime.ts,
            t: firstDoc.postImageOpTime.t
        });
        assert.eq(afterDoc, savedDoc.o);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        lastTs = firstDoc.ts;

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (replacement update, return pre-image)

        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: 40},
            update: {y: 1},
            new: false,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        beforeDoc = mainConn.getDB('test').user.findOne({_id: 40});
        res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 40, ts: {$gt: lastTs}});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        assert.eq(null, firstDoc.postImageTs);

        savedDoc = oplog.findOne({
            ns: 'test.user',
            op: 'n',
            ts: firstDoc.preImageOpTime.ts,
            t: firstDoc.preImageOpTime.t
        });
        assert.eq(beforeDoc, savedDoc.o);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        lastTs = firstDoc.ts;

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (replacement update, return post-image)

        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: 40},
            update: {z: 1},
            new: true,
            upsert: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));
        afterDoc = mainConn.getDB('test').user.findOne({_id: 40});

        firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 40, ts: {$gt: lastTs}});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        assert.eq(null, firstDoc.preImageTs);

        savedDoc = oplog.findOne({
            ns: 'test.user',
            op: 'n',
            ts: firstDoc.postImageOpTime.ts,
            t: firstDoc.postImageOpTime.t
        });
        assert.eq(afterDoc, savedDoc.o);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        lastTs = firstDoc.ts;

        ////////////////////////////////////////////////////////////////////////
        // Test findAndModify command (remove, return pre-image)

        incrementTxnNumber();
        cmd = {
            findAndModify: 'user',
            query: {_id: 40},
            remove: true,
            new: false,
            lsid: {id: lsid},
            txnNumber: txnNumber,
        };

        beforeDoc = mainConn.getDB('test').user.findOne({_id: 40});
        res = assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 40, ts: {$gt: lastTs}});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        assert.eq(null, firstDoc.postImageTs);

        savedDoc = oplog.findOne({
            ns: 'test.user',
            op: 'n',
            ts: firstDoc.preImageOpTime.ts,
            t: firstDoc.preImageOpTime.t
        });
        assert.eq(beforeDoc, savedDoc.o);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        lastTs = firstDoc.ts;
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
