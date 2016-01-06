'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
    // Disabled due to known bugs

    // Disabled because the TTL monitor can delete documents in the background while we're comparing
    // dbhashes between the primary and secondaries (SERVER-21881).
    'indexed_insert_ttl.js',

    // Disabled due to MongoDB restrictions and/or workload restrictions
    'agg_group_external.js', // uses >100MB of data, which can overwhelm test hosts
    'agg_sort_external.js', // uses >100MB of data, which can overwhelm test hosts
].map(function(file) { return dir + '/' + file; });

runWorkloadsSerially(ls(dir).filter(function(file) {
    return !Array.contains(blacklist, file);
}), { replication: true });
