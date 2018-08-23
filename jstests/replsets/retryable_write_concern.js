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

    let replTest = new ReplSetTest({nodes: kNodes});
    replTest.startSet({verbose: 1});
    replTest.initiate();

    let priConn = replTest.getPrimary();
    let secConn = replTest.getSecondary();

    let lsid = UUID();

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      insert: 'user',
                                      documents: [{_id: 10}, {_id: 30}],
                                      ordered: false,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(34),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      update: 'user',
                                      updates: [
                                          {q: {_id: 10}, u: {$inc: {x: 1}}},
                                      ],
                                      ordered: false,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(35),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      delete: 'user',
                                      deletes: [{q: {x: 1}, limit: 1}, {q: {y: 1}, limit: 1}],
                                      ordered: false,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(36),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      findAndModify: 'user',
                                      query: {_id: 60},
                                      update: {$inc: {x: 1}},
                                      new: true,
                                      upsert: true,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(37),
                                      writeConcern: {w: 'majority', wtimeout: 200},
                                    },
                                    kNodes);

    runWriteConcernRetryabilityTest(priConn,
                                    secConn,
                                    {
                                      commitTransaction: 1,
                                      lsid: {id: lsid},
                                      txnNumber: NumberLong(38),
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
                                            txnNumber: NumberLong(38),
                                            readConcern: {level: 'snapshot'},
                                            autocommit: false,
                                            startTransaction: true
                                        }));

                                    });

    replTest.stopSet();
})();
