// Storage Node Watchdog test cases
// - Validate set parameter functions correctly.
(function() {
    'use strict';
    const admin = db.getSiblingDB("admin");

    // Check the defaults are correct
    //
    function getparam(adminDb, field) {
        let q = {getParameter: 1};
        q[field] = 1;

        const ret = adminDb.runCommand(q);
        return ret[field];
    }

    // Verify the defaults are as we documented them
    assert.eq(getparam(admin, "watchdogPeriodSeconds"), -1);

    function setparam(adminDb, obj) {
        const ret = adminDb.runCommand(Object.extend({setParameter: 1}, obj));
        return ret;
    }

    // Negative tests
    // Negative: set it too low.
    assert.commandFailed(setparam(admin, {"watchdogPeriodSeconds": 1}));
    // Negative: set it the min value but fail since it was not enabled.
    assert.commandFailed(setparam(admin, {"watchdogPeriodSeconds": 60}));
    // Negative: set it the min value + 1 but fail since it was not enabled.
    assert.commandFailed(setparam(admin, {"watchdogPeriodSeconds": 61}));

    // Now test MongoD with it enabled at startup
    //
    const conn = MongoRunner.runMongod({setParameter: "watchdogPeriodSeconds=60"});
    assert.neq(null, conn, 'mongod was unable to start up');

    const admin2 = conn.getDB("admin");

    // Validate defaults
    assert.eq(getparam(admin2, "watchdogPeriodSeconds"), 60);

    // Negative: set it too low.
    assert.commandFailed(setparam(admin2, {"watchdogPeriodSeconds": 1}));
    // Positive: set it the min value
    assert.commandWorked(setparam(admin2, {"watchdogPeriodSeconds": 60}));
    // Positive: set it the min value + 1
    assert.commandWorked(setparam(admin2, {"watchdogPeriodSeconds": 61}));

    // Positive: disable it
    assert.commandWorked(setparam(admin2, {"watchdogPeriodSeconds": -1}));

    assert.eq(getparam(admin2, "watchdogPeriodSeconds"), -1);

    // Positive: enable it again
    assert.commandWorked(setparam(admin2, {"watchdogPeriodSeconds": 60}));

    MongoRunner.stopMongod(conn);

})();
