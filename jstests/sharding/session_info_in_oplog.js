/**
 * Test that the oplog entries will contain the correct logical session id, transaction number and
 * statement id after executing a write command. Also tests that the session table is properly
 * updated after the write operations.
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
const kNodes = 2;

var checkOplog = function(oplog, lsid, uid, txnNum, stmtId, prevTs, prevTerm) {
    assert(oplog != null);
    assert(oplog.lsid != null);
    assert.eq(lsid, oplog.lsid.id);
    assert.eq(uid, oplog.lsid.uid);
    assert.eq(txnNum, oplog.txnNumber);
    if (typeof (stmtId) !== 'undefined')
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

var runTests = function(mainConn, priConn, secConn) {
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
    // Test single insert command

    var cmd = {
        insert: 'user',
        documents: [{_id: 50}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    var oplog = priConn.getDB('local').oplog.rs;

    var firstDoc = oplog.findOne({ns: 'test.user', 'o._id': 50});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);

    ////////////////////////////////////////////////////////////////////////
    // Test multiple insert command

    incrementTxnNumber();
    var cmd = {
        insert: 'user',
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    oplog = priConn.getDB('local').oplog.rs;

    if (FeatureFlagUtil.isPresentAndEnabled(priConn, "ReplicateVectoredInsertsTransactionally")) {
        firstDoc = oplog.findOne({
            $and: [
                {"o.applyOps": {$elemMatch: {ns: 'test.user', 'o._id': 10}}},
                {"o.applyOps": {$elemMatch: {ns: 'test.user', 'o._id': 30}}}
            ]
        });
        checkOplog(firstDoc, lsid, uid, txnNumber, undefined /* stmtId */, Timestamp(0, 0), -1);
        // Statement IDs are defined on the inner operation for vectored inserts.
        assert.eq(firstDoc.o.applyOps[0].stmtId, 0);
        assert.eq(firstDoc.o.applyOps[1].stmtId, 1);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
        checkSessionCatalog(secConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    } else {
        firstDoc = oplog.findOne({ns: 'test.user', 'o._id': 10});
        checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

        var secondDoc = oplog.findOne({ns: 'test.user', 'o._id': 30});
        checkOplog(secondDoc, lsid, uid, txnNumber, 1, firstDoc.ts, firstDoc.t);

        checkSessionCatalog(priConn, lsid, uid, txnNumber, secondDoc.ts, secondDoc.t);
        checkSessionCatalog(secConn, lsid, uid, txnNumber, secondDoc.ts, secondDoc.t);
    }

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
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 10});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    secondDoc = oplog.findOne({ns: 'test.user', op: 'i', 'o._id': 20});
    checkOplog(secondDoc, lsid, uid, txnNumber, 1, firstDoc.ts, firstDoc.t);

    var thirdDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 30});
    checkOplog(thirdDoc, lsid, uid, txnNumber, 2, secondDoc.ts, secondDoc.t);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, thirdDoc.ts, thirdDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, thirdDoc.ts, thirdDoc.t);

    ////////////////////////////////////////////////////////////////////////
    // Test delete command

    incrementTxnNumber();
    cmd = {
        delete: 'user',
        deletes: [{q: {_id: 10}, limit: 1}, {q: {_id: 20}, limit: 1}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    firstDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 10});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    secondDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 20});
    checkOplog(secondDoc, lsid, uid, txnNumber, 1, firstDoc.ts, firstDoc.t);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, secondDoc.ts, secondDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, secondDoc.ts, secondDoc.t);

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
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    firstDoc = oplog.findOne({ns: 'test.user', op: 'i', 'o._id': 40});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    assert.eq(null, firstDoc.preImageTs);
    assert.eq(null, firstDoc.postImageTs);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    var lastTs = firstDoc.ts;

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (in-place update)

    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: 40},
        update: {$inc: {x: 1}},
        new: false,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 40, ts: {$gt: lastTs}});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    lastTs = firstDoc.ts;

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (replacement update)

    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: 40},
        update: {y: 1},
        new: false,
        upsert: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    firstDoc = oplog.findOne({ns: 'test.user', op: 'u', 'o2._id': 40, ts: {$gt: lastTs}});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    lastTs = firstDoc.ts;

    ////////////////////////////////////////////////////////////////////////
    // Test findAndModify command (remove)

    incrementTxnNumber();
    cmd = {
        findAndModify: 'user',
        query: {_id: 40},
        remove: true,
        new: false,
        lsid: {id: lsid},
        txnNumber: txnNumber,
        writeConcern: {w: kNodes},
    };

    assert.commandWorked(mainConn.getDB('test').runCommand(cmd));

    firstDoc = oplog.findOne({ns: 'test.user', op: 'd', 'o._id': 40, ts: {$gt: lastTs}});
    checkOplog(firstDoc, lsid, uid, txnNumber, 0, Timestamp(0, 0), -1);

    checkSessionCatalog(priConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    checkSessionCatalog(secConn, lsid, uid, txnNumber, firstDoc.ts, firstDoc.t);
    lastTs = firstDoc.ts;
};

// This test specifically looks for side-effects of writing retryable findAndModify images into the
// oplog as noops. Ensure images are not stored in a side collection.
var replTest = new ReplSetTest({nodes: kNodes});
replTest.startSet();
replTest.initiate();

var priConn = replTest.getPrimary();
var secConn = replTest.getSecondary();
secConn.setSecondaryOk();

runTests(priConn, priConn, secConn);

replTest.stopSet();

var st = new ShardingTest({shards: {rs0: {nodes: kNodes}}});

secConn = st.rs0.getSecondary();
secConn.setSecondaryOk();
runTests(st.s, st.rs0.getPrimary(), secConn);

st.stop();
