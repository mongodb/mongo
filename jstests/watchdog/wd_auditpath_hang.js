// Storage Node Watchdog - validate watchdog monitors --auditpath
//
load("jstests/watchdog/lib/wd_test_common.js");

(function() {
    'use strict';

    if (assert.commandWorked(db.runCommand({buildInfo: 1})).modules.includes("enterprise")) {
        let control = new CharybdefsControl("auditpath_hang");

        const auditPath = control.getMountPath();

        testFuseAndMongoD(control, {

            auditDestination: 'file',
            auditFormat: 'JSON',
            auditPath: auditPath + "/auditLog.json"
        });
    }

})();
