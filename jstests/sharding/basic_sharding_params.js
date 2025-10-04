/**
 * Test of complex sharding initialization
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function shardingTestUsingObjects() {
    let st = new ShardingTest({
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
                    d4: {verbose: 2},
                },
            },
        },
    });

    let s0 = st.s0;
    assert.eq(s0, st._mongos[0]);

    let s1 = st.s1;
    assert.eq(s1, st._mongos[1]);

    let c0 = st.c0;
    assert.eq(c0, st.configRS.nodes[0]);

    let rs0 = st.rs0;
    assert.eq(rs0, st._rsObjects[0]);

    let rs1 = st.rs1;
    assert.eq(rs1, st._rsObjects[1]);

    let rs0_d0 = rs0.nodes[0];

    let rs1_d0 = rs1.nodes[0];
    let rs1_a1 = rs1.nodes[1];

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
    let st = new ShardingTest({
        mongos: [{verbose: 5}, {verbose: 4}],
        config: [{verbose: 3}],
        shards: [{verbose: 2}, {verbose: 1}],
    });

    let s0 = st.s0;
    assert.eq(s0, st._mongos[0]);

    let s1 = st.s1;
    assert.eq(s1, st._mongos[1]);

    let c0 = st.c0;
    assert.eq(c0, st.configRS.nodes[0]);

    let rs0 = st.rs0;
    assert.eq(rs0, st._rsObjects[0]);

    let rs1 = st.rs1;
    assert.eq(rs1, st._rsObjects[1]);

    let rs0_d0 = rs0.nodes[0];

    let rs1_d0 = rs1.nodes[0];

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
