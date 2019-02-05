// Tests that it is illegal to read from system.views within a transaction.
// @tags: [uses_transactions]
(function() {
    "use strict";

    load("jstests/libs/fixture_helpers.js");  // For 'FixtureHelpers'.

    const session = db.getMongo().startSession({causalConsistency: false});

    // Use a custom database to avoid conflict with other tests that use system.views.
    const testDB = session.getDatabase("no_reads_from_system_dot_views_in_txn");
    assert.commandWorked(testDB.dropDatabase());

    testDB.runCommand({create: "foo", viewOn: "bar", pipeline: []});

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(testDB.runCommand({find: "system.views", filter: {}}), 51071);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

    if (FixtureHelpers.isMongos(testDB)) {
        // The rest of the test is concerned with a find by UUID which is not supported against
        // mongos.
        return;
    }

    const collectionInfos =
        new DBCommandCursor(testDB, assert.commandWorked(testDB.runCommand({listCollections: 1})));
    let systemViewsUUID = null;
    while (collectionInfos.hasNext()) {
        const next = collectionInfos.next();
        if (next.name === "system.views") {
            systemViewsUUID = next.info.uuid;
        }
    }
    assert.neq(null, systemViewsUUID, "did not find UUID for system.views");

    session.startTransaction({readConcern: {level: "snapshot"}});
    assert.commandFailedWithCode(testDB.runCommand({find: systemViewsUUID, filter: {}}), 51070);
    assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                 ErrorCodes.NoSuchTransaction);

}());
