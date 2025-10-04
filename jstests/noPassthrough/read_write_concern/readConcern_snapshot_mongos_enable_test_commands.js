// Verifies that snapshot readConcern on mongos is not gated by the enableTestCommands flag.
//
// @tags: [
//    disables_test_commands,
//    requires_sharding,
// ]
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = "test";
const collName = "coll";

// Runs multiple commands with read concern level "snapshot" in a session,
// expecting success.
function expectSnapshotReadConcernIsSupported() {
    const st = new ShardingTest({shards: 1, config: 1});
    const session = st.s.startSession({causalConsistency: false});
    let txnNumber = 0;

    assert.commandWorked(
        session.getDatabase(dbName).runCommand({
            find: collName,
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++),
            startTransaction: true,
            autocommit: false,
        }),
    );

    assert.commandWorked(
        session.getDatabase(dbName).runCommand({
            aggregate: collName,
            pipeline: [],
            cursor: {},
            readConcern: {level: "snapshot"},
            txnNumber: NumberLong(txnNumber++),
            startTransaction: true,
            autocommit: false,
        }),
    );

    session.endSession();
    st.stop();
}

// Snapshot readConcern should succeed when 'enableTestCommands' is set to false.
TestData.enableTestCommands = false;
expectSnapshotReadConcernIsSupported();

// Snapshot readConcern should succeed when 'enableTestCommands' is set to true.
TestData.enableTestCommands = true;
expectSnapshotReadConcernIsSupported();
