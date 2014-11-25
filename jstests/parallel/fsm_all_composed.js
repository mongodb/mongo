load('jstests/parallel/fsm_libs/runner.js');

var dir = 'jstests/parallel/fsm_workloads';

var blacklist = [
    'indexed_insert_multikey.js', // SERVER-16143
    'indexed_insert_multikey_noindex.js' // SERVER-16143
].map(function(file) { return dir + '/' + file; });

runMixtureOfWorkloads(ls(dir).filter(function(file) {
    return !Array.contains(blacklist, file);
}));
