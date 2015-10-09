'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var dir = 'jstests/concurrency/fsm_workloads';

var blacklist = [
].map(function(file) { return dir + '/' + file; });

// SERVER-16196 re-enable executing workloads
// runWorkloadsInParallel(ls(dir).filter(function(file) {
//     return !Array.contains(blacklist, file);
// }));
