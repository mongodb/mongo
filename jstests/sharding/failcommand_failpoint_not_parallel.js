(function() {
    "use strict";

    const st = new ShardingTest({shards: 3, mongos: 1});
    const db = st.s.getDB("test_failcommand_noparallel");

    // Test times when closing connection.
    // Sharding tests require failInternalCommands: true, since the mongos appears to mongod to be
    // an internal client.
    assert.commandWorked(st.s.adminCommand({
        configureFailPoint: "failCommand",
        mode: {times: 2},
        data: {closeConnection: true, failCommands: ["find"], failInternalCommands: true}
    }));
    assert.throws(() => db.runCommand({find: "c"}));
    assert.throws(() => db.runCommand({find: "c"}));
    assert.commandWorked(db.runCommand({find: "c"}));
    assert.commandWorked(st.s.adminCommand({configureFailPoint: "failCommand", mode: "off"}));

    st.stop();
}());
