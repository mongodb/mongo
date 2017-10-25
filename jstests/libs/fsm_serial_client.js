// This is the template file used in Powercycle testing for launching FSM Serial clients.
'use strict';

load('jstests/concurrency/fsm_libs/runner.js');

var workloadDir = 'jstests/concurrency/fsm_workloads';

var workloadList = TestData.workloadFiles || ls(workloadDir);
var dbNamePrefix = TestData.dbNamePrefix || '';
var fsmDbBlacklist = TestData.fsmDbBlacklist || [];
var validateCollectionsOnCleanup = TestData.validateCollections;

runWorkloadsSerially(workloadList, {}, {dbNamePrefix: dbNamePrefix}, {
    keepExistingDatabases: true,
    dropDatabaseBlacklist: fsmDbBlacklist,
    validateCollections: validateCollectionsOnCleanup
});
