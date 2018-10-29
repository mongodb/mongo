// Verifies transactions can run with majority read concern, with or without afterClusterTime.
//
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "majority_read_concern";

    function runTest(sessionOptions) {
        jsTestLog("Testing transactions with majority read concern and sessionOptions: " +
                  tojson(sessionOptions));

        db.getSiblingDB(dbName).runCommand({drop: collName, writeConcern: {w: "majority"}});

        const session = db.getMongo().startSession(sessionOptions);
        const sessionColl = session.getDatabase(dbName)[collName];

        // Set up the collection.
        assert.writeOK(sessionColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));

        // Assert a transaction can run with the given session options and read concern level
        // majority.
        session.startTransaction({readConcern: {level: "majority"}});
        assert.eq(1, sessionColl.find().itcount());
        assert.commandWorked(session.commitTransaction_forTesting());

        session.endSession();
    }

    runTest({causalConsistency: false});
    runTest({causalConsistency: true});
}());
