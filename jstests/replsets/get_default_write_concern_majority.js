/**
 * This tests the behavior of changing the default write concern to the implicit default if there is
 * no CWWC.
 * @tags: [requires_fcv_50]
 */
(function() {
'use strict';
load("jstests/libs/write_concern_util.js");  // For isDefaultWriteConcernMajorityFlagEnabled.

jsTestLog("Test PSS configuration will set defaultWC to majority.");
let replTest = new ReplSetTest({name: 'default_wc_majority', nodes: 3});
replTest.startSet();
replTest.initiate();
let primary = replTest.getPrimary();

let res = assert.commandWorked(primary.adminCommand({getDefaultRWConcern: 1}));
if (isDefaultWriteConcernMajorityFlagEnabled(primary)) {
    assert(res.hasOwnProperty("defaultWriteConcern"));
    assert.eq({w: "majority", wtimeout: 0}, res.defaultWriteConcern, tojson(res));
} else {
    assert(!res.hasOwnProperty("defaultWriteConcern"));
}

replTest.stopSet();

jsTestLog("Test PSA configuration will set defaultWC to {w:1}.");
replTest = new ReplSetTest({name: 'default_wc_w_1', nodes: [{}, {}, {arbiter: true}]});
replTest.startSet();
replTest.initiate();
primary = replTest.getPrimary();

res = assert.commandWorked(primary.adminCommand({getDefaultRWConcern: 1}));
assert(!res.hasOwnProperty("defaultWriteConcern"));

replTest.stopSet();
})();
