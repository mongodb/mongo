// Test that 'notablescan' parameter does not affect queries internal namespaces.
// @tags: [uses_transactions]
(function() {
"use strict";

const dbName = "test";
const collName = "coll";

function runTests(ServerType) {
    const s = new ServerType();

    const configDB = s.getConn().getDB("config");
    const session = s.getConn().getDB(dbName).getMongo().startSession();
    const primaryDB = session.getDatabase(dbName);

    // Implicitly create the collection outside of the transaction.
    assert.commandWorked(primaryDB.getCollection(collName).insert({x: 1}));

    // Run a transaction so the 'config.transactions' collection is implicitly created.
    session.startTransaction();
    assert.commandWorked(primaryDB.getCollection(collName).insert({x: 2}));
    assert.commandWorked(session.commitTransaction_forTesting());

    // Run a predicate query that would fail if we did not ignore the 'notablescan' flag.
    assert.eq(configDB.transactions.find({any_nonexistent_field: {$exists: true}}).itcount(), 0);

    // Run the same query against the user created collection honoring the 'notablescan' flag.
    // This will cause the query to fail as there is no viable query plan. Unfortunately,
    // the reported query error code is the cryptic 'BadValue'.
    assert.commandFailedWithCode(
        primaryDB.runCommand({find: collName, filter: {any_nonexistent_field: {$exists: true}}}),
        ErrorCodes.NoQueryExecutionPlans);

    s.stop();
}

function Sharding() {
    this.st = new ShardingTest({
        shards: 2,
        config: 1,
        other: {
            shardOptions: {setParameter: {notablescan: true}},
            configOptions: {setParameter: {notablescan: true}}
        }
    });
}

Sharding.prototype.stop = function() {
    this.st.stop();
};

Sharding.prototype.getConn = function() {
    return this.st.s0;
};

function ReplSet() {
    this.rst = new ReplSetTest({nodes: 1, nodeOptions: {setParameter: {notablescan: true}}});
    this.rst.startSet();
    this.rst.initiate();
}

ReplSet.prototype.stop = function() {
    this.rst.stopSet();
};

ReplSet.prototype.getConn = function() {
    return this.rst.getPrimary();
};

[ReplSet, Sharding].forEach(runTests);
}());
