// Verifies which read concern levels transactions support, with and without afterClusterTime.
//
// @tags: [uses_transactions, uses_snapshot_read_concern, requires_majority_read_concern]
(function() {
"use strict";

const dbName = "test";
const collName = "supported_read_concern_levels";

function runTest(level, sessionOptions, supported) {
    jsTestLog("Testing transactions with read concern level: " + level +
              " and sessionOptions: " + tojson(sessionOptions));

    db.getSiblingDB(dbName).runCommand({drop: collName, writeConcern: {w: "majority"}});

    const session = db.getMongo().startSession(sessionOptions);
    const sessionDB = session.getDatabase(dbName);
    const sessionColl = sessionDB[collName];

    // Set up the collection.
    assert.commandWorked(sessionColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));

    if (level) {
        session.startTransaction({readConcern: {level: level}});
    } else {
        session.startTransaction();
    }

    const res = sessionDB.runCommand({find: collName});
    if (supported) {
        assert.commandWorked(res,
                             "expected success, read concern level: " + level +
                                 ", sessionOptions: " + tojson(sessionOptions));
        assert.commandWorked(session.commitTransaction_forTesting());
    } else {
        assert.commandFailedWithCode(res,
                                     ErrorCodes.InvalidOptions,
                                     "expected failure, read concern level: " + level +
                                         ", sessionOptions: " + tojson(sessionOptions));
        assert.commandFailedWithCode(session.abortTransaction_forTesting(),
                                     ErrorCodes.NoSuchTransaction);
    }

    session.endSession();
}

// Starting a txn with no read concern level is allowed.
runTest(undefined, {causalConsistency: false}, true /*supported*/);
runTest(undefined, {causalConsistency: true}, true /*supported*/);

const kSupportedLevels = ["local", "majority", "snapshot"];
for (let level of kSupportedLevels) {
    runTest(level, {causalConsistency: false}, true /*supported*/);
    runTest(level, {causalConsistency: true}, true /*supported*/);
}

const kUnsupportedLevels = ["available", "linearizable"];
for (let level of kUnsupportedLevels) {
    runTest(level, {causalConsistency: false}, false /*supported*/);
    runTest(level, {causalConsistency: true}, false /*supported*/);
}
}());
