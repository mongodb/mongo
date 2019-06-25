/**
 * Tests for making sure that retried writes will wait properly for writeConcern.
 *
 * @tags: [uses_transactions, uses_prepare_transaction]
 */
(function() {

    "use strict";

    load("jstests/libs/retryable_writes_util.js");
    load("jstests/libs/write_concern_util.js");
    load("jstests/libs/feature_compatibility_version.js");

    if (!RetryableWritesUtil.storageEngineSupportsRetryableWrites(jsTest.options().storageEngine)) {
        jsTestLog("Retryable writes are not supported, skipping test");
        return;
    }

    const kNodes = 2;

    let replTest = new ReplSetTest({nodes: kNodes});
    replTest.startSet({verbose: 1});
    replTest.initiate();

    let priConn = replTest.getPrimary();
    let secConn = replTest.getSecondary();

    // Stopping replication on secondaries can take up to 5 seconds normally. Set a small oplog
    // getMore timeout so the test runs faster.
    assert.commandWorked(secConn.adminCommand(
        {configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}));

    let lsid = UUID();

    // Start at an arbitrary txnNumber.
    let txnNumber = 31;

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      insert: 'user',
                                      documents: [{_id: 10}, {_id: 30}],
                                      ordered: false,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      update: 'user',
                                      updates: [
                                          {q: {_id: 10}, u: {$inc: {x: 1}}},
                                      ],
                                      ordered: false,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      delete: 'user',
                                      deletes: [{q: {x: 1}, limit: 1}, {q: {y: 1}, limit: 1}],
                                      ordered: false,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      findAndModify: 'user',
                                      query: {_id: 60},
                                      update: {$inc: {x: 1}},
                                      new: true,
                                      upsert: true,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      setFeatureCompatibilityVersion: lastStableFCV,
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes,
                                    'admin');
    assert.commandWorked(priConn.adminCommand({setFeatureCompatibilityVersion: lastStableFCV}));
    checkFCV(priConn.getDB('admin'), lastStableFCV);

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      setFeatureCompatibilityVersion: latestFCV,
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes,
                                    'admin');
    assert.commandWorked(priConn.adminCommand({setFeatureCompatibilityVersion: latestFCV}));
    checkFCV(priConn.getDB('admin'), latestFCV);

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      commitTransaction: 1,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      autocommit: false,
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes,
                                    'admin',
                                    function(conn) {
                                        assert.commandWorked(conn.getDB('test').runCommand({
                                            insert: 'user',
                                            documents: [{_id: 80}, {_id: 90}],
                                            ordered: false,
                                            lsid: {id: lsid},
                                            txnNumber: NumberLong(txnNumber),
                                            readConcern: {level: 'snapshot'},
                                            autocommit: false,
                                            startTransaction: true
                                        }));

                                    });

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      prepareTransaction: 1,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      autocommit: false,
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes,
                                    'admin',
                                    function(conn) {
                                        assert.commandWorked(conn.getDB('test').runCommand({
                                            insert: 'user',
                                            documents: [{_id: 100}, {_id: 110}],
                                            ordered: false,
                                            lsid: {id: lsid},
                                            txnNumber: NumberLong(txnNumber),
                                            readConcern: {level: 'snapshot'},
                                            autocommit: false,
                                            startTransaction: true
                                        }));
                                    });
    assert.commandWorked(priConn.adminCommand({
        abortTransaction: 1,
        lsid: {id: lsid},
        txnNumber: NumberLong(txnNumber),
        autocommit: false,
        writeConcern: {w: 'majority'},
    }));

    txnNumber++;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      abortTransaction: 1,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      autocommit: false,
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes,
                                    'admin',
                                    function(conn) {
                                        assert.commandWorked(conn.getDB('test').runCommand({
                                            insert: 'user',
                                            documents: [{_id: 120}, {_id: 130}],
                                            ordered: false,
                                            lsid: {id: lsid},
                                            txnNumber: NumberLong(txnNumber),
                                            readConcern: {level: 'snapshot'},
                                            autocommit: false,
                                            startTransaction: true
                                        }));
                                        assert.commandWorked(conn.adminCommand({
                                            prepareTransaction: 1,
                                            lsid: {id: lsid},
                                            txnNumber: NumberLong(txnNumber),
                                            autocommit: false,
                                            writeConcern: {w: 'majority'},
                                        }));
                                    });

    txnNumber++;
    assert.commandWorked(priConn.getDB('test').runCommand({
        insert: 'user',
        documents: [{_id: 140}, {_id: 150}],
        ordered: false,
        lsid: {id: lsid},
        txnNumber: NumberLong(txnNumber),
        readConcern: {level: 'snapshot'},
        autocommit: false,
        startTransaction: true
    }));
    const prepareTS = assert
                          .commandWorked(priConn.adminCommand({
                              prepareTransaction: 1,
                              lsid: {id: lsid},
                              txnNumber: NumberLong(txnNumber),
                              autocommit: false,
                              writeConcern: {w: 'majority'},
                          }))
                          .prepareTimestamp;
    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      commitTransaction: 1,
                                      commitTimestamp: prepareTS,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(txnNumber),
                                      autocommit: false,
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes,
                                    'admin');

    replTest.stopSet();
})();
