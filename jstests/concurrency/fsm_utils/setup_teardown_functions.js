'use strict';

/**
 * Functions used by runners to set up and tear down their clusters.
 * Each function is called by executeOnMongodNodes and executeOnMongosNodes
 * (if the cluster is sharded). Each function should accept a connection to
 * the 'admin' database.
 */

var increaseDropDistLockTimeout = function increaseDropDistLockTimeout(db) {
    var waitTimeSecs = 10 * 60;  // 10 minutes
    assert.commandWorked(db.runCommand({
        configureFailPoint: 'setDropCollDistLockWait',
        mode: 'alwaysOn',
        data: {waitForSecs: waitTimeSecs}
    }));
};

var resetDropDistLockTimeout = function resetDropDistLockTimeout(db) {
    assert.commandWorked(
        db.runCommand({configureFailPoint: 'setDropCollDistLockWait', mode: 'off'}));
};

var setYieldAllLocksFailPoint = function setYieldAllLocksFailPoint(db) {
    var waitTimeMillis = 20;
    assert.commandWorked(db.runCommand({
        configureFailPoint: 'setYieldAllLocksWait',
        mode: 'alwaysOn',
        data: {waitForMillis: waitTimeMillis}
    }));
};

var resetYieldAllLocksFailPoint = function resetYieldAllLocksFailPoint(db) {
    assert.commandWorked(db.runCommand({configureFailPoint: 'setYieldAllLocksWait', mode: 'off'}));
};
