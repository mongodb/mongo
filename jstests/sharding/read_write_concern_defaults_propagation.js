// Tests propagation of RWC defaults across a sharded cluster.
//
// @tags: [requires_fcv_44]

load("jstests/libs/read_write_concern_defaults_propagation.js");

(function() {
'use strict';

var st = new ShardingTest({
    shards: 1,
    mongos: 3,
    config: 3,
    other: {
        rs: true,
        rs0: {nodes: 1},
    }
});

// TODO: after SERVER-43720 is done, remove this line and uncomment the below lines.
ReadWriteConcernDefaultsPropagation.runTests(st.configRS.getPrimary(), [st.configRS.getPrimary()]);

// const mongosAndConfigNodes = [st.s0, st.s1, st.s2, ...st.configRS.nodes];
// ReadWriteConcernDefaultsPropagation.runTests(st.s0, mongosAndConfigNodes);

st.stop();
})();
