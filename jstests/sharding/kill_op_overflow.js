/**
 * This test asserts that an illegal OpID passed to bongos' implementation of killOp results in a
 * failure being propagated back to the client.
 */
(function() {
    "use strict";
    var st = new ShardingTest({name: "shard1", shards: 1, bongos: 1});

    assert.commandFailed(
        st.s.getDB("admin").runCommand({killOp: 1, op: "shard0000:99999999999999999999999"}));
})();
