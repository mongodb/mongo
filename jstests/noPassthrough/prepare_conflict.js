/**
 * Tests that prepare conflicts for prepared transactions are retried.
 * We use a failpoint after the prepare command to pause and test document visibility.
 *
 * TODO: At this point, prepare is only able to abort after transactions. This tests assumes the
 * completion of a prepare operation results in an unmodified state.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";
    let replSet = new ReplSetTest({
        name: "prepareReadConflicts",
        nodes: 3,
    });

    replSet.startSet();
    replSet.initiate();
    replSet.awaitSecondaryNodes();

    const collName = "prepare_read_conflict";
    let testDB = replSet.getPrimary().getDB("test");
    let adminDB = replSet.getPrimary().getDB("admin");
    let testColl = testDB.getCollection(collName);

    testColl.drop();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Starts a session, does an update to a document, prepares the transaction, pauses from the set
    // failpoint, then aborts when the failpoint is unset.
    // The update operation increments 'x' by one on the document 'doc'.
    function doUpdateTransactionPrepare(conn, query, update, txn) {
        TestData.collName = collName;
        TestData.query = query;
        TestData.update = update;
        TestData.txnNumber = txn;
        let shellFn = function() {
            // Create the session.
            var session = db.getMongo().startSession({causalConsistency: false});
            var sessionDB = session.getDatabase('test');
            // Update, but don't autocommit
            assert.commandWorked(sessionDB.runCommand({
                update: TestData.collName,
                updates: [{q: TestData.query, u: TestData.update}],
                txnNumber: NumberLong(TestData.txnNumber),
                readConcern: {level: 'snapshot'},
                autocommit: false,
                startTransaction: true
            }));
            // Run prepare, which blocks until the failpoint is unset.
            assert.commandWorked(sessionDB.adminCommand({
                prepareTransaction: 1,
                txnNumber: NumberLong(TestData.txnNumber),
                autocommit: false
            }));
        };

        return startParallelShell(shellFn, conn.port);
    }

    // Read until a prepare conflict is encountered
    function waitForPrepareConflict(conn, filter) {
        assert.soon(function() {
            let res = testDB.runCommand({find: collName, filter: filter, maxTimeMS: 1000});

            // Do a short sleep if there is no error.
            if (res.ok) {
                sleep(10);
                return false;
            }

            // The profiler should log a prepare read conflict when the query times out.
            let cur =
                testDB.system.profile.find({"ns": "test." + collName, "command.filter": filter});
            while (cur.hasNext()) {
                let n = cur.next();
                print("op: " + JSON.stringify(n));
                if (n.prepareReadConflicts > 0) {
                    return true;
                }
            }

            // In this case the command failed but did not generate any prepare conflicts.
            return false;
        });
    }

    // Insert the document.
    const doc1 = {_id: 1, x: 1};
    assert.writeOK(testColl.insert(doc1));

    // Enable the profiler to log slow queries. We expect a 'find' to hang until the prepare
    // conflict is resolved.
    testDB.runCommand({profile: 1, level: 1, slowms: 100});

    // Enable the failpoint to pause after running prepare.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'pauseAfterTransactionPrepare', mode: 'alwaysOn'}));

    // Start a parallel shell to update, prepare, and pause;
    const txnNumber = 0;
    let prepareAwait =
        doUpdateTransactionPrepare(replSet.getPrimary(), doc1, {$inc: {x: 1}}, txnNumber);

    waitForPrepareConflict(replSet.getPrimary().conn, {_id: 1});

    // At this point, we can guarantee all subsequent reads will conflict. Do a read in a parallel
    // shell, disable the failpoint, then ensure the read succeeded with the old document.
    TestData.collName = collName;
    let findAwait = startParallelShell(function() {
        var it = db.getSiblingDB('test').runCommand({find: TestData.collName, filter: {_id: 1}});
    }, replSet.getPrimary().port);

    // Disable the failpoint to let the transaction proceed.
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'pauseAfterTransactionPrepare', mode: 'off'}));

    // The find command should be successful.
    findAwait({checkExitSuccess: true});

    // Wait for prepare to exit successfully.
    prepareAwait({checkExitSuccess: true});

    // The document should be unmodified, because prepare does not yet commit.
    assert.eq(doc1, testColl.findOne(doc1));

    replSet.stopSet();
})();
