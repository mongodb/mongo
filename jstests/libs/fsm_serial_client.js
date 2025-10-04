// This is the template file used in Powercycle testing for launching FSM Serial clients.
import {runWorkloadsSerially} from "jstests/concurrency/fsm_libs/runner.js";

let workloadDir = "jstests/concurrency/fsm_workloads";

let workloadList = TestData.workloadFiles || ls(workloadDir);
let workloadDenylist = TestData.workloadDenylistFiles || [];
let dbNamePrefix = TestData.dbNamePrefix || "";
let fsmDbDenylist = TestData.fsmDbDenylist || [];
let validateCollectionsOnCleanup = TestData.validateCollections;

let denylist = workloadDenylist.map(function (file) {
    return workloadDir + "/" + file;
});

await runWorkloadsSerially(
    workloadList.filter(function (file) {
        return !Array.contains(denylist, file);
    }),
    {},
    {dbNamePrefix: dbNamePrefix},
    {
        keepExistingDatabases: true,
        dropDatabaseDenylist: fsmDbDenylist,
        validateCollections: validateCollectionsOnCleanup,
    },
);
