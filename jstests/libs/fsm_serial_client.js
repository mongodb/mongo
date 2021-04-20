// This is the template file used in Powercycle testing for launching FSM Serial clients.
'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var workloadDir = 'jstests/concurrency/fsm_workloads';

var workloadList = TestData.workloadFiles || ls(workloadDir);
var workloadDenylist = TestData.workloadDenylistFiles || [];
var dbNamePrefix = TestData.dbNamePrefix || '';
var fsmDbDenylist = TestData.fsmDbDenylist || [];
var validateCollectionsOnCleanup = TestData.validateCollections;

var denylist = workloadDenylist.map(function(file) {
    return workloadDir + '/' + file;
});

runWorkloadsSerially(workloadList.filter(function(file) {
    return !Array.contains(denylist, file);
}),
                     {},
                     {dbNamePrefix: dbNamePrefix},
                     {
                         keepExistingDatabases: true,
                         dropDatabaseDenylist: fsmDbDenylist,
                         validateCollections: validateCollectionsOnCleanup
                     });
