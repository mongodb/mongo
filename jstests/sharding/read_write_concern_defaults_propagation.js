// Tests propagation of RWC defaults across a sharded cluster.
// @tags: [
//   # Test doesn't start enough mongods to have num_mongos routers
//   temp_disabled_embedded_router,
// ]
import {
    ReadWriteConcernDefaultsPropagation
} from "jstests/libs/read_write_concern_defaults_propagation_common.js";

var st = new ShardingTest({
    shards: 1,
    mongos: 3,
    other: {
        rs: true,
        rs0: {nodes: 1},
    },
    // The config server needs enough nodes to allow a custom write concern default of w:2
    config: 3,
});

const mongosAndConfigNodes = [st.s0, st.s1, st.s2, ...st.configRS.nodes];
ReadWriteConcernDefaultsPropagation.runTests(st.s0, mongosAndConfigNodes);

// Verify the in-memory defaults are updated correctly. This verifies the cache is invalidated
// properly on secondaries when an update to the defaults document is replicated because the
// in-memory value will only be updated after an invalidation.
ReadWriteConcernDefaultsPropagation.runTests(st.s0, mongosAndConfigNodes, true /* inMemory */);

ReadWriteConcernDefaultsPropagation.runDropAndDeleteTests(st.s0, mongosAndConfigNodes);

ReadWriteConcernDefaultsPropagation.runDropAndDeleteTests(st.configRS.getPrimary(),
                                                          mongosAndConfigNodes);

st.stop();