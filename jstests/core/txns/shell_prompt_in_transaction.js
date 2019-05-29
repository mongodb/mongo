// Test shell prompt doesn't run in the session of the global 'db'.
// @tags: [uses_transactions]

(function() {
    "use strict";

    const collName = "shell_prompt_in_transaction";

    db.getCollection(collName).drop({writeConcern: {w: "majority"}});
    assert.commandWorked(db.runCommand({create: collName, writeConcern: {w: "majority"}}));

    // Override the global "db".
    const session = db.getMongo().startSession();
    db = session.getDatabase(db.getName());
    const coll = db.getCollection(collName);

    function simulatePrompt() {
        __promptWrapper__(defaultPrompt);
    }

    // Start a transaction, so the session will attach txn info to the commands running on it.
    session.startTransaction();
    jsTestLog("Run shell prompt to simulate a user hitting enter.");
    simulatePrompt();
    const doc = {_id: "shell-write"};
    assert.commandWorked(coll.insert(doc));
    assert.docEq(doc, coll.findOne());
    simulatePrompt();
    assert.commandWorked(session.abortTransaction_forTesting());
    assert.docEq(null, coll.findOne());

    // Start a transaction, so the session has a running transaction now.
    simulatePrompt();
    session.startTransaction();
    jsTestLog("Run shell prompt to simulate a user hitting enter.");
    simulatePrompt();
    assert.commandWorked(coll.insert(doc));
    simulatePrompt();
    session.commitTransaction();
    assert.docEq(doc, coll.findOne());

    coll.drop({writeConcern: {w: "majority"}});
})();
