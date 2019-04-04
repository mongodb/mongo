(function() {
    "use strict";

    const st = new ShardingTest({shards: 3, merizos: 1});
    const db = st.s.getDB("test_failcommand_noparallel");

    // Test times when closing connection.
    // Sharding tests require failInternalCommands: true, since the merizos appears to merizod to be
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
