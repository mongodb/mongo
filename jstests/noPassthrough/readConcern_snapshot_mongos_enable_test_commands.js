// Verifies that snapshot readConcern on mongos is gated by the enableTestCommands flag.
//
// @tags: [requires_sharding]
(function() {
    "use strict";

    const dbName = "test";
    const collName = "coll";

    // Snapshot readConcern should fail when 'enableTestCommands' is set to false.
    {
        jsTest.setOption("enableTestCommands", false);

        const st = new ShardingTest({shards: 1, config: 1});
        const session = st.s.startSession({causalConsistency: false});
        let txnNumber = 0;

        assert.commandFailedWithCode(session.getDatabase(dbName).runCommand({
            find: collName,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++),
            startTransaction: true,
            autocommit: false
        }),
                                     ErrorCodes.InvalidOptions);

        assert.commandFailedWithCode(session.getDatabase(dbName).runCommand({
            aggregate: collName,
            pipeline: [],
            cursor: {},
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++),
            startTransaction: true,
            autocommit: false
        }),
                                     ErrorCodes.InvalidOptions);

        session.endSession();
        st.stop();
    }

    // Snapshot readConcern should succeed when 'enableTestCommands' is set to true.
    {
        jsTest.setOption("enableTestCommands", true);

        const st = new ShardingTest({shards: 1, config: 1});
        const session = st.s.startSession({causalConsistency: false});
        let txnNumber = 0;

        assert.commandWorked(session.getDatabase(dbName).runCommand({
            find: collName,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++),
            startTransaction: true,
            autocommit: false
        }));

        assert.commandWorked(session.getDatabase(dbName).runCommand({
            aggregate: collName,
            pipeline: [],
            cursor: {},
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++),
            startTransaction: true,
            autocommit: false
        }));

        session.endSession();
        st.stop();
    }
}());
