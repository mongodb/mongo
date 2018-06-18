/**
 * Tests that prepare conflicts for prepared transactions are retried.
 *
 * @tags: [uses_transactions]
 */
(function() {
    "use strict";

    const dbName = "test";
    const collName = "prepare_conflict";
    const testDB = db.getSiblingDB(dbName);
    const testColl = testDB.getCollection(collName);

    testColl.drop();
    assert.commandWorked(testDB.runCommand({create: collName, writeConcern: {w: "majority"}}));

    function assertPrepareConflict(filter) {
        assert.commandFailedWithCode(
            testDB.runCommand({find: collName, filter: filter, maxTimeMS: 1000}),
            ErrorCodes.ExceededTimeLimit);

        let prepareConflicted = false;
        const cur =
            testDB.system.profile.find({"ns": testColl.getFullName(), "command.filter": filter});
        while (cur.hasNext()) {
            const n = cur.next();
            print("op: " + JSON.stringify(n));
            if (n.prepareReadConflicts > 0) {
                prepareConflicted = true;
            }
        }
        assert(prepareConflicted);
    }

    // Insert the document.
    const doc1 = {_id: 1, x: 1};
    assert.commandWorked(testColl.insert(doc1));

    // Enable the profiler to log slow queries. We expect a 'find' to hang until the prepare
    // conflict is resolved.
    assert.commandWorked(testDB.runCommand({profile: 1, level: 1, slowms: 100}));

    const session = db.getMongo().startSession({causalConsistency: false});
    const sessionDB = session.getDatabase(dbName);
    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandWorked(sessionDB.runCommand({
        update: collName,
        updates: [{q: doc1, u: {$inc: {x: 1}}}],
    }));

    assert.commandWorked(sessionDB.adminCommand({prepareTransaction: 1}));
    assertPrepareConflict({_id: 1});

    // At this point, we can guarantee all subsequent reads will conflict. Do a read in a parallel
    // shell, abort the transaction, then ensure the read succeeded with the old document.
    TestData.collName = collName;
    TestData.dbName = dbName;
    const findAwait = startParallelShell(function() {
        const it = db.getSiblingDB(TestData.dbName)
                       .runCommand({find: TestData.collName, filter: {_id: 1}});
    }, db.getMongo().port);

    session.abortTransaction();

    // The find command should be successful.
    findAwait({checkExitSuccess: true});

    // The document should be unmodified, because we aborted.
    assert.eq(doc1, testColl.findOne(doc1));
})();
