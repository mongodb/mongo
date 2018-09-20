/**
 * Tests that it is possible to step down a primary while there are transactions in prepare.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";
    load("jstests/core/txns/libs/prepare_helpers.js");
    load("jstests/replsets/rslib.js");  // For reconnect()

    const replTest = new ReplSetTest({nodes: 1});
    replTest.startSet();
    replTest.initiate();

    const priConn = replTest.getPrimary();
    const lsid = UUID();
    const dbName = jsTest.name();
    const collName = jsTest.name();
    const testDB = priConn.getDB(dbName);

    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    jsTestLog("Starting basic transaction");

    const session = priConn.startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    session.startTransaction();

    assert.commandWorked(sessionDB.getCollection(collName).insert({a: 1}));

    jsTestLog("Putting transaction into prepare");
    PrepareHelpers.prepareTransaction(session);

    replTest.awaitReplication();

    jsTestLog("Stepping down primary");

    // Force stepdown primary since there are no secondaries.
    assert.throws(function() {
        priConn.adminCommand({replSetStepDown: 60, force: true});
    });

    reconnect(priConn);
    assert(!assert.commandWorked(priConn.adminCommand({ismaster: 1})).isMaster);
    assert.eq(ReplSetTest.State.SECONDARY,
              assert.commandWorked(priConn.adminCommand({replSetGetStatus: 1})).myState);

    replTest.stopSet(null /*signal*/, false /*forRestart*/, {skipValidation: true});
})();
