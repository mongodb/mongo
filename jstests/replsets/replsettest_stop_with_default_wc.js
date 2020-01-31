/**
 * Tests that none of the operations in the ReplSetTest consistency checks are affected by
 * changing the write concern default during the test itself.
 *
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

const name = jsTestName();

// We need to have at least 2 nodes to run the data consistency checks.
const rst = new ReplSetTest({name: name, nodes: 2});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
// We deliberately set an unsatisfiable writeConcern here, in order to make it easier to distinguish
// what WC the consistency checks are using.
assert.commandWorked(primary.adminCommand({setDefaultRWConcern: 1, defaultWriteConcern: {w: 42}}));

// It should always be possible to successfully stop the replset (including running consistency
// checks) even when the default writeConcern is unsatisfiable.
rst.stopSet();
})();
