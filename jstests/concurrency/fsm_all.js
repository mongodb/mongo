'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
    // Disabled due to MongoDB restrictions and/or workload restrictions

    // This workload assumes it is running against a sharded cluster.
    'sharded_moveChunk_drop_shard_key_index.js',
].map(function(file) {
    return dir + '/' + file;
});

runWorkloadsSerially(ls(dir).filter(function(file) {
    return !Array.contains(blacklist, file);
}));
