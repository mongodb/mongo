/**
 * Test basic retryable write without errors by checking that the resulting collection after the
 * retry is as expected and it does not create additional oplog entries.
 */
(function() {
    "use strict";

    load("jstests/libs/retryable_writes_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    function checkFindAndModifyResult(expected, toCheck) {
        assert.eq(expected.ok, toCheck.ok);
        assert.eq(expected.value, toCheck.value);
        assert.docEq(expected.lastErrorObject, toCheck.lastErrorObject);
    }

    function runTests(mainConn, priConn) {
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
    }

    function runFailpointTests(mainConn, priConn) {
        // Test the 'onPrimaryTransactionalWrite' failpoint
        var lsid = UUID();
        var testDb = mainConn.getDB('TestDB');

        // Test connection close (default behaviour). The connection will get closed, but the
        // inserts must succeed
        assert.commandWorked(priConn.adminCommand(
            {configureFailPoint: 'onPrimaryTransactionalWrite', mode: 'alwaysOn'}));

        try {
            // Set skipRetryOnNetworkError so the shell doesn't automatically retry, since the
            // command has a txnNumber.
            TestData.skipRetryOnNetworkError = true;
            var res = assert.commandWorked(testDb.runCommand({
                insert: 'user',
                documents: [{x: 0}, {x: 1}],
                ordered: true,
                lsid: {id: lsid},
                txnNumber: NumberLong(1)
            }));
            // Mongos will automatically retry on retryable errors if the request has a txnNumber,
            // and the retry path for already completed writes does not trigger the failpoint, so
            // the command will succeed when run through mongos.
            assert.eq(2, res.n);
            assert.eq(false, res.hasOwnProperty("writeErrors"));
        } catch (e) {
            var exceptionMsg = e.toString();
            assert(isNetworkError(e), 'Incorrect exception thrown: ' + exceptionMsg);
        } finally {
            TestData.skipRetryOnNetworkError = false;
        }

        assert.eq(2, testDb.user.find({}).itcount());

        // Test exception throw. One update must succeed and the other must fail.
        assert.commandWorked(priConn.adminCommand({
            configureFailPoint: 'onPrimaryTransactionalWrite',
            mode: {skip: 1},
            data: {
                closeConnection: false,
                failBeforeCommitExceptionCode: ErrorCodes.InternalError
            }
        }));

        var cmd = {
            update: 'user',
            updates: [{q: {x: 0}, u: {$inc: {y: 1}}}, {q: {x: 1}, u: {$inc: {y: 1}}}],
            ordered: true,
            lsid: {id: lsid},
            txnNumber: NumberLong(2)
        };

        var writeResult = testDb.runCommand(cmd);

        assert.eq(1, writeResult.nModified);
        assert.eq(1, writeResult.writeErrors.length);
        assert.eq(1, writeResult.writeErrors[0].index);
        assert.eq(ErrorCodes.InternalError, writeResult.writeErrors[0].code);

        assert.commandWorked(
            priConn.adminCommand({configureFailPoint: 'onPrimaryTransactionalWrite', mode: 'off'}));

        var writeResult = testDb.runCommand(cmd);
        assert.eq(2, writeResult.nModified);

        var collContents = testDb.user.find({}).sort({x: 1}).toArray();
        assert.eq(2, collContents.length);
        assert.eq(0, collContents[0].x);
        assert.eq(1, collContents[0].y);
        assert.eq(1, collContents[1].x);
        assert.eq(1, collContents[1].y);
    }

    function runMultiTests(mainConn, priConn) {
        // Test the behavior of retryable writes with multi=true / limit=0
        var lsid = {id: UUID()};
        var testDb = mainConn.getDB('test_multi');

        // Only the update statements with multi=true in a batch fail.
        var cmd = {
            update: 'user',
            updates: [{q: {x: 1}, u: {y: 1}}, {q: {x: 2}, u: {z: 1}, multi: true}],
            ordered: true,
            lsid: lsid,
            txnNumber: NumberLong(1),
        };
        var res = assert.commandWorkedIgnoringWriteErrors(testDb.runCommand(cmd));
        assert.eq(1,
                  res.writeErrors.length,
                  'expected only one write error, received: ' + tojson(res.writeErrors));
        assert.eq(1,
                  res.writeErrors[0].index,
                  'expected the update at index 1 to fail, not the update at index: ' +
                      res.writeErrors[0].index);
        assert.eq(ErrorCodes.InvalidOptions,
                  res.writeErrors[0].code,
                  'expected to fail with code ' + ErrorCodes.InvalidOptions + ', received: ' +
                      res.writeErrors[0].code);

        // Only the delete statements with limit=0 in a batch fail.
        cmd = {
            delete: 'user',
            deletes: [{q: {x: 1}, limit: 1}, {q: {y: 1}, limit: 0}],
            ordered: false,
            lsid: lsid,
            txnNumber: NumberLong(1),
        };
        res = assert.commandWorkedIgnoringWriteErrors(testDb.runCommand(cmd));
        assert.eq(1,
                  res.writeErrors.length,
                  'expected only one write error, received: ' + tojson(res.writeErrors));
        assert.eq(1,
                  res.writeErrors[0].index,
                  'expected the delete at index 1 to fail, not the delete at index: ' +
                      res.writeErrors[0].index);
        assert.eq(ErrorCodes.InvalidOptions,
                  res.writeErrors[0].code,
                  'expected to fail with code ' + ErrorCodes.InvalidOptions + ', received: ' +
                      res.writeErrors[0].code);
    }

    // Tests for replica set
    var replTest = new ReplSetTest({nodes: 2});
    replTest.startSet();
    replTest.initiate();

    var priConn = replTest.getPrimary();

    runTests(priConn, priConn);
    runFailpointTests(priConn, priConn);
    runMultiTests(priConn, priConn);

    replTest.stopSet();

    // Tests for sharded cluster
    var st = new ShardingTest({shards: {rs0: {nodes: 1}}});

    runTests(st.s0, st.rs0.getPrimary());
    runFailpointTests(st.s0, st.rs0.getPrimary());
    runMultiTests(st.s0, st.rs0.getPrimary());

    st.stop();
})();
