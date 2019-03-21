// Verifies targeting errors encountered in a transaction lead to write errors.
//
// @tags: [uses_transactions]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "foo";
    const ns = dbName + "." + collName;

    const st = new ShardingTest({shards: 2});
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {skey: "hashed"}}));

    const session = st.s.startSession();
    const sessionDB = session.getDatabase("test");

    // Failed update.

    session.startTransaction();

    let res = sessionDB.runCommand(
        {update: collName, updates: [{q: {skey: {$lte: 5}}, u: {$set: {x: 1}}, multi: false}]});
    assert.commandFailedWithCode(res, ErrorCodes.InvalidOptions);
    assert(res.hasOwnProperty("writeErrors"), "expected write errors, res: " + tojson(res));

    session.abortTransaction_forTesting();

    // Failed delete.

    session.startTransaction();

    res = sessionDB.runCommand({delete: collName, deletes: [{q: {skey: {$lte: 5}}, limit: 1}]});
    assert.commandFailedWithCode(res, ErrorCodes.ShardKeyNotFound);
    assert(res.hasOwnProperty("writeErrors"), "expected write errors, res: " + tojson(res));

    session.abortTransaction_forTesting();

    st.stop();
}());
