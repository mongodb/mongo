/**
 * Test that attempting to call getParameter on the featureCompatibilityVersion before the fCV is
 * initialized does not crash the server (see SERVER-34600).
 */
(function() {
'use strict';

let rst = new ReplSetTest({nodes: 2});
rst.startSet();
let node = rst.nodes[0];

// The featureCompatibilityVersion parameter is initialized during rst.initiate(), so calling
// getParameter on the fCV before then will attempt to access an uninitialized fCV.

const getParamCmd = {
    getParameter: 1,
    featureCompatibilityVersion: 1
};
assert.commandFailedWithCode(
    node.getDB('admin').runCommand(getParamCmd),
    ErrorCodes.UnknownFeatureCompatibilityVersion,
    'expected ' + tojson(getParamCmd) + ' to fail with code UnknownFeatureCompatibilityVersion');

rst.initiate();

// After the replica set is initialized, getParameter should successfully return the fCV.

const primary = rst.getPrimary();
const res = primary.adminCommand(getParamCmd);
assert.commandWorked(res);
assert.eq(res.featureCompatibilityVersion.version, latestFCV, tojson(res));

rst.stopSet();
})();
