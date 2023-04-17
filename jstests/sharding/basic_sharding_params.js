/**
 * Test of complex sharding initialization
 */

(function() {
'use strict';

function shardingTestUsingObjects() {
    var st = new ShardingTest({
        mongos: {s0: {verbose: 6}, s1: {verbose: 5}},
        config: {c0: {verbose: 4}},
        shards: {
            d0: {verbose: 3},
            rs1: {
                nodes: {
                    d0: {verbose: 2},
                    a1: {verbose: 1},
                    d2: {verbose: 2},
                    d3: {verbose: 2},
                    d4: {verbose: 2}
                }
            }
        }
    });

    var s0 = st.s0;
    assert.eq(s0, st._mongos[0]);

    var s1 = st.s1;
    assert.eq(s1, st._mongos[1]);

    var c0 = st.c0;
    assert.eq(c0, st.configRS.nodes[0]);

    var rs0 = st.rs0;
    assert.eq(rs0, st._rsObjects[0]);

    var rs1 = st.rs1;
    assert.eq(rs1, st._rsObjects[1]);

    var rs0_d0 = rs0.nodes[0];

    var rs1_d0 = rs1.nodes[0];
    var rs1_a1 = rs1.nodes[1];

    assert(s0.commandLine.hasOwnProperty("vvvvvv"));
    assert(s1.commandLine.hasOwnProperty("vvvvv"));
    if (!TestData.configShard) {
        assert(c0.commandLine.hasOwnProperty("vvvv"));
    } else {
        // Same as shard 1.
        assert(c0.commandLine.hasOwnProperty("vvv"));
    }
    assert(rs0_d0.commandLine.hasOwnProperty("vvv"));
    assert(rs1_d0.commandLine.hasOwnProperty("vv"));
    assert(rs1_a1.commandLine.hasOwnProperty("v"));

    st.stop();
}

function shardingTestUsingArrays() {
    var st = new ShardingTest({
        mongos: [{verbose: 5}, {verbose: 4}],
        config: [{verbose: 3}],
        shards: [{verbose: 2}, {verbose: 1}]
    });

    var s0 = st.s0;
    assert.eq(s0, st._mongos[0]);

    var s1 = st.s1;
    assert.eq(s1, st._mongos[1]);

    var c0 = st.c0;
    assert.eq(c0, st.configRS.nodes[0]);

    var rs0 = st.rs0;
    assert.eq(rs0, st._rsObjects[0]);

    var rs1 = st.rs1;
    assert.eq(rs1, st._rsObjects[1]);

    var rs0_d0 = rs0.nodes[0];

    var rs1_d0 = rs1.nodes[0];

    assert(s0.commandLine.hasOwnProperty("vvvvv"));
    assert(s1.commandLine.hasOwnProperty("vvvv"));
    if (!TestData.configShard) {
        assert(c0.commandLine.hasOwnProperty("vvv"));
    } else {
        // Same as shard 1.
        assert(c0.commandLine.hasOwnProperty("vv"));
    }
    assert(rs0_d0.commandLine.hasOwnProperty("vv"));
    assert(rs1_d0.commandLine.hasOwnProperty("v"));

    st.stop();
}

shardingTestUsingObjects();
shardingTestUsingArrays();
})();
