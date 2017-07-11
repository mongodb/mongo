/**
 * Test that the oplog entries will contain the correct logical session id, transaction number and
 * statement id after executing a write command.
 */
(function() {
    "use strict";

    var checkOplog = function(mainConn, priConn) {
        var lsid = UUID();

        var cmd = {
            insert: 'user',
            documents: [{_id: 10}, {_id: 30}],
            ordered: false,
            lsid: {id: {uuid: lsid}},
            txnNumber: NumberLong(34),
        };

        assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

        var oplog = priConn.getDB('local').oplog.rs;

        var firstDoc = oplog.findOne({ns: 'test.user', 'o._id': 10});
        assert(firstDoc != null);
        assert(firstDoc.lsid != null);
        assert.eq(lsid, firstDoc.lsid.id.uuid);
        assert.eq(NumberLong(34), firstDoc.txnNumber);
        assert.eq(0, firstDoc.stmtId);

        var secondDoc = oplog.findOne({ns: 'test.user', 'o._id': 30});
        assert(secondDoc != null);
        assert(secondDoc.lsid != null);
        assert.eq(lsid, secondDoc.lsid.id.uuid);
        assert.eq(NumberLong(34), secondDoc.txnNumber);
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
