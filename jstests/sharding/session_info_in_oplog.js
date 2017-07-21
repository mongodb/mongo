/**
 * Test that the oplog entries will contain the correct logical session id, transaction number and
 * statement id after executing a write command.
 */
(function() {
    "use strict";

    var checkOplog = function(mainConn, priConn) {
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
        assert(firstDoc != null);
        assert(firstDoc.lsid != null);
        assert.eq(lsid, firstDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(34), firstDoc.txnNumber);
        assert.eq(0, firstDoc.stmtId);

        var secondDoc = oplog.findOne({ns: 'test.user', 'o._id': 30});
        assert(secondDoc != null);
        assert(secondDoc.lsid != null);
        assert.eq(lsid, secondDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(34), secondDoc.txnNumber);
        assert.eq(1, secondDoc.stmtId);

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
        assert(firstDoc != null);
        assert(firstDoc.lsid != null);
        assert.eq(lsid, firstDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(35), firstDoc.txnNumber);
        assert.eq(0, firstDoc.stmtId);

        secondDoc = oplog.findOne({ns: 'test.user', op: 'i', 'o._id': 20});
        assert(secondDoc != null);
        assert(secondDoc.lsid != null);
        assert.eq(lsid, secondDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(35), secondDoc.txnNumber);
        assert.eq(1, secondDoc.stmtId);

        var thirdDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 30});
        assert(thirdDoc != null);
        assert(thirdDoc.lsid != null);
        assert.eq(lsid, thirdDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(35), thirdDoc.txnNumber);
        assert.eq(2, thirdDoc.stmtId);

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
        assert(firstDoc != null);
        assert(firstDoc.lsid != null);
        assert.eq(lsid, firstDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(36), firstDoc.txnNumber);
        assert.eq(0, firstDoc.stmtId);

        secondDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 20});
        assert(secondDoc != null);
        assert(secondDoc.lsid != null);
        assert.eq(lsid, secondDoc.lsid.id);
        assert.eq(uid, firstDoc.lsid.uid);
        assert.eq(NumberLong(36), secondDoc.txnNumber);
        assert.eq(1, secondDoc.stmtId);
    };

    var replTest = new ReplSetTest({nodes: 1});
    replTest.startSet({verbose: 1});
    replTest.initiate();

    var priConn = replTest.getPrimary();

    checkOplog(priConn, priConn);

    replTest.stopSet();

    var st = new ShardingTest({shards: {rs0: {nodes: 1}}});

    checkOplog(st.s, st.rs0.getPrimary());

    st.stop();

})();
