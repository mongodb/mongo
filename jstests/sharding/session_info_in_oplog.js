/**
 * Test that the oplog entries will contain the correct logical session id, transaction number and
 * statement id after executing a write command. Also tests that the session table is properly
 * updated after the write operations.
 */
(function() {
    "use strict";

    var checkOplog = function(oplog, lsid, uid, txnNum, stmtId, prevTs) {
        assert(oplog != null);
        assert(oplog.lsid != null);
        assert.eq(lsid, oplog.lsid.id);
        assert.eq(uid, oplog.lsid.uid);
        assert.eq(txnNum, oplog.txnNumber);
        assert.eq(stmtId, oplog.stmtId);
        assert.eq(prevTs.getTime(), oplog.prevTs.getTime());
        assert.eq(prevTs.getInc(), oplog.prevTs.getInc());
    };

    var checkSessionCatalog = function(conn, sessionId, uid, txnNum, expectedTs) {
        var coll = conn.getDB('config').transactions;
        var sessionDoc = coll.findOne({'_id': {id: sessionId, uid: uid}});

        assert.eq(txnNum, sessionDoc.txnNum);
        assert.eq(expectedTs.getTime(), sessionDoc.lastWriteOpTimeTs.getTime());
        assert.eq(expectedTs.getInc(), sessionDoc.lastWriteOpTimeTs.getInc());
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

        ////////////////////////////////////////////////////////////////////////
        // Test insert command

        var cmd = {
            insert: 'user',
            documents: [{_id: 10}, {_id: 30}],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(34),
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        var oplog = priConn.getDB('local').oplog.rs;

        var firstDoc = oplog.findOne({ns: 'test.user', 'o._id': 10});
        checkOplog(firstDoc, lsid, uid, NumberLong(34), 0, Timestamp(0, 0));

        var secondDoc = oplog.findOne({ns: 'test.user', 'o._id': 30});
        checkOplog(secondDoc, lsid, uid, NumberLong(34), 1, firstDoc.ts);

        checkSessionCatalog(priConn, lsid, uid, NumberLong(34), secondDoc.ts);

        ////////////////////////////////////////////////////////////////////////
        // Test update command

        cmd = {
            update: 'user',
            updates: [
                {q: {_id: 10}, u: {$set: {x: 1}}},  // in place
                {q: {_id: 20}, u: {$set: {y: 1}}, upsert: true},
                {q: {_id: 30}, u: {z: 1}}  // replacement
            ],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(35),
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 10});
        checkOplog(firstDoc, lsid, uid, NumberLong(35), 0, Timestamp(0, 0));

        secondDoc = oplog.findOne({ns: 'test.user', op: 'i', 'o._id': 20});
        checkOplog(secondDoc, lsid, uid, NumberLong(35), 1, firstDoc.ts);

        var thirdDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 30});
        checkOplog(thirdDoc, lsid, uid, NumberLong(35), 2, secondDoc.ts);

        checkSessionCatalog(priConn, lsid, uid, NumberLong(35), thirdDoc.ts);

        ////////////////////////////////////////////////////////////////////////
        // Test delete command

        cmd = {
            delete: 'user',
            deletes: [{q: {_id: 10}, limit: 1}, {q: {_id: 20}, limit: 1}],
            ordered: false,
            lsid: {id: lsid},
            txnNumber: NumberLong(36),
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        firstDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 10});
        checkOplog(firstDoc, lsid, uid, NumberLong(36), 0, Timestamp(0, 0));

        secondDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 20});
        checkOplog(secondDoc, lsid, uid, NumberLong(36), 1, firstDoc.ts);

        checkSessionCatalog(priConn, lsid, uid, NumberLong(36), secondDoc.ts);
    };

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet({verbose: 1});
    replTest.initiate();

    var priConn = replTest.getPrimary();

    runTests(priConn, priConn);

    replTest.stopSet();

    var st = new ShardingTest({shards: {rs0: {nodes: 1}}});

    runTests(st.s, st.rs0.getPrimary());

    st.stop();

})();
