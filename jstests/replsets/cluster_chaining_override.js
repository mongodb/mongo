/**
 * Tests that if chaining is disabled, enabling the server parameter
 * 'enableOverrideClusterChainingSetting' will allow the node to chain anyway.
 * @tags: [requires_fcv_50]
 */

(function() {
"use strict";
load("jstests/replsets/rslib.js");

let rst = new ReplSetTest({
    nodes: {
        n0: {},
        n1: {rsConfig: {priority: 0}},
        n2: {rsConfig: {priority: 0}, setParameter: {enableOverrideClusterChainingSetting: true}}
    },
    // Enable the periodic noop writer to aid sync source selection.
    nodeOptions: {setParameter: {writePeriodicNoops: true}},
    settings: {chainingAllowed: false},
    useBridge: true
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
rst.awaitSecondaryNodes();
const [n1, n2] = rst.getSecondaries();

// Create a network partition between n2 and the primary.
n2.disconnect(primary);

// Since n2 has enabled the parameter 'enableOverrideClusterChainingSetting', n2 should eventually
// chain from n1.
rst.awaitSyncSource(n2, n1);

// A write with write concern {w:3} should still reach n2.
var options = {writeConcern: {w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}};
assert.commandWorked(primary.getDB("admin").foo.insert({x: 1}, options));

rst.stopSet();
}());