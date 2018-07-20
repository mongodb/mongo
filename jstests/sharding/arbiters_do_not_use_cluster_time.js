/**
 * Tests that arbiters do not gossip clusterTime or operationTime.
 */

(function() {
    "use strict";
    let st = new ShardingTest(
        {shards: {rs0: {nodes: [{arbiter: false}, {arbiter: false}, {arbiter: true}]}}});

    jsTestLog("Started ShardingTest");

    let secondaries = st.rs0.getSecondaries();

    let foundArbiter = false;
    for (let i = 0; i < secondaries.length; i++) {
        let conn = secondaries[i].getDB("admin");
        const res = conn.runCommand({isMaster: 1});
        if (res["arbiterOnly"]) {
            assert(!foundArbiter);
            foundArbiter = true;
            // nodes with disabled clocks do not gossip clusterTime and operationTime.
            assert.eq(res.hasOwnProperty("$clusterTime"), false);
            assert.eq(res.hasOwnProperty("operationTime"), false);
        } else {
            assert.eq(res.hasOwnProperty("$clusterTime"), true);
            assert.eq(res.hasOwnProperty("operationTime"), true);
        }
    }
    assert.eq(foundArbiter, true);
    st.stop();
})();
