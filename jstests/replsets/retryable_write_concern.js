/**
 * Tests for making sure that retried writes will wait properly for writeConcern.
 *
 * @tags: [uses_transactions]
 */
(function() {

    "use strict";

    load("jstests/libs/retryable_writes_util.js");
    load("jstests/libs/write_concern_util.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    const kNodes = 2;

    let checkWriteConcernTimedOut = function(result) {
        let wcErr = result.writeConcernError;
        assert.neq(null, wcErr, tojson(result));

        let errInfo = wcErr.errInfo;
        assert.neq(null, errInfo, tojson(result));

        assert.eq(true, errInfo.wtimeout, tojson(result));
    };

    /**
     * Tests that a retryable write properly waits for writeConcern on retry. Takes an optional
     * 'setupFunc' that sets up the database state. 'setupFunc' accepts a connection to the
     * primary.
     */
    let runTest = function(priConn, secConn, cmd, dbName, setupFunc) {
        dbName = dbName || "test";
        jsTestLog(`Testing ${tojson(cmd)} on ${dbName}.`);

        // Send a dummy write to this connection so it will have the Client object initialized.
        let secondPriConn = new Mongo(priConn.host);
        let testDB2 = secondPriConn.getDB(dbName);
        assert.writeOK(testDB2.dummy.insert({x: 1}, {writeConcern: {w: kNodes}}));

        if (setupFunc) {
            setupFunc(priConn);
        }

        stopServerReplication(secConn);

        let testDB = priConn.getDB(dbName);
        checkWriteConcernTimedOut(testDB.runCommand(cmd));
        checkWriteConcernTimedOut(testDB2.runCommand(cmd));

        restartServerReplication(secConn);
    };

    let replTest = new ReplSetTest({nodes: kNodes});
    replTest.startSet({verbose: 1});
    replTest.initiate();

    let priConn = replTest.getPrimary();
    let secConn = replTest.getSecondary();

    let lsid = UUID();

    runTest(priConn, secConn, {
        insert: 'user',
        documents: [{_id: 10}, {_id: 30}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(34),
        writeConcern: {w: 'majority', wtimeout: 200},
    });

    runTest(priConn, secConn, {
        update: 'user',
        updates: [
            {q: {_id: 10}, u: {$inc: {x: 1}}},
        ],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(35),
        writeConcern: {w: 'majority', wtimeout: 200},
    });

    runTest(priConn, secConn, {
        delete: 'user',
        deletes: [{q: {x: 1}, limit: 1}, {q: {y: 1}, limit: 1}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(36),
        writeConcern: {w: 'majority', wtimeout: 200},
    });

    runTest(priConn, secConn, {
        findAndModify: 'user',
        query: {_id: 60},
        update: {$inc: {x: 1}},
        new: true,
        upsert: true,
        lsid: {id: lsid},
        txnNumber: NumberLong(37),
        writeConcern: {w: 'majority', wtimeout: 200},
    });

    runTest(priConn,
            secConn,
            {
              commitTransaction: 1,
              lsid: {id: lsid},
              txnNumber: NumberLong(38),
              autocommit: false,
              writeConcern: {w: 'majority', wtimeout: 200},
            },
            'admin',
            function(conn) {
                assert.commandWorked(conn.getDB('test').runCommand({
                    insert: 'user',
                    documents: [{_id: 80}, {_id: 90}],
                    ordered: false,
                    lsid: {id: lsid},
                    txnNumber: NumberLong(38),
                    readConcern: {level: 'snapshot'},
                    autocommit: false,
                    startTransaction: true
                }));

            });

    replTest.stopSet();
})();
