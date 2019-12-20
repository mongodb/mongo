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

const mongosAndConfigNodes = [st.s0, st.s1, st.s2, ...st.configRS.nodes];
ReadWriteConcernDefaultsPropagation.runTests(st.s0, mongosAndConfigNodes);

// Verify the in-memory defaults are updated correctly. This verifies the cache is invalidated
// properly on secondaries when an update to the defaults document is replicated because the
// in-memory value will only be updated after an invalidation.
ReadWriteConcernDefaultsPropagation.runTests(st.s0, [...mongosAndConfigNodes], true /* inMemory */);

// TODO SERVER-45282: When the defaults document is deleted, later lookups with find a document with
// no epoch, so the current defaults will not be overwritten on a mongos. After this is resolved,
// this case should use "mongosAndConfigNodes" as the checkConns.
ReadWriteConcernDefaultsPropagation.runDropAndDeleteTests(st.s0, [...st.configRS.nodes]);

// TODO SERVER-45282: When the defaults document is deleted, later lookups with find a document with
// no epoch, so the current defaults will not be overwritten on a mongos. After this is resolved,
// this case should use "mongosAndConfigNodes" as the checkConns.
ReadWriteConcernDefaultsPropagation.runDropAndDeleteTests(st.configRS.getPrimary(),
                                                          [...st.configRS.nodes]);

st.stop();
})();
