'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
    // Disabled due to MongoDB restrictions and/or workload restrictions
    'agg_group_external.js', // uses >100MB of data, which can overwhelm test hosts
    'agg_sort_external.js', // uses >100MB of data, which can overwhelm test hosts
].map(function(file) { return dir + '/' + file; });

runWorkloadsSerially(ls(dir).filter(function(file) {
    return !Array.contains(blacklist, file);
}), { replication: true });
