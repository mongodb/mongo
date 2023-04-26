/**
 * Test that the 'rollbackTimeLimitSecs' server parameter can be set both on startup and at runtime,
 * and that we only accept positive numbers as values for it. This number represents the maximum
 * allowed timespan of the rollback period, which is calculated using the wall clock times of oplog
 * entries.
 */

(function() {

"use strict";

const testName = "rollback_time_limit_param";

// Make sure that we reject non-positive values for this parameter set on startup.
let rstWithBadStartupOptions = new ReplSetTest(
    {name: testName, nodes: 1, nodeOptions: {setParameter: "rollbackTimeLimitSecs=-50"}});

assert.throws(function() {
    rstWithBadStartupOptions.startSet();
});

assert(rawMongoProgramOutput().match("Bad value for parameter \"rollbackTimeLimitSecs\""),
       "failed to reject bad value for parameter");

// Now initialize the same parameter correctly on startup.
let rst = new ReplSetTest(
    {name: testName, nodes: 1, nodeOptions: {setParameter: "rollbackTimeLimitSecs=1000"}});
rst.startSet();
rst.initiate();

let primary = rst.getPrimary();

// Check that the value of 'rollbackTimeLimitSecs' was initialized correctly on startup.
let valueSetOnStartup =
    assert.commandWorked(primary.adminCommand({getParameter: 1, rollbackTimeLimitSecs: 1}))
        .rollbackTimeLimitSecs;
assert.eq(NumberLong(1000), valueSetOnStartup);

// Check that the value of 'rollbackTimeLimitSecs' was set correctly at runtime.
assert.commandWorked(primary.adminCommand({setParameter: 1, rollbackTimeLimitSecs: 2000}));
let valueSetAtRuntime =
    assert.commandWorked(primary.adminCommand({getParameter: 1, rollbackTimeLimitSecs: 1}))
        .rollbackTimeLimitSecs;
assert.eq(NumberLong(2000), valueSetAtRuntime);

// Make sure that we reject non-positive values for this parameter set at runtime.
assert.commandFailedWithCode(primary.adminCommand({setParameter: 1, rollbackTimeLimitSecs: -5}),
                             ErrorCodes.BadValue);
assert.commandFailedWithCode(primary.adminCommand({setParameter: 1, rollbackTimeLimitSecs: 0}),
                             ErrorCodes.BadValue);

rst.stopSet();
})();
