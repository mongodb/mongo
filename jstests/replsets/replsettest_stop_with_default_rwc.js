/**
 * Tests that none of the operations in the ReplSetTest consistency checks are affected by
 * changing the default read or write concern during the test itself.
 */
(function() {
"use strict";

const name = jsTestName();

// We need to have at least 2 nodes to run the data consistency checks.
const rst = new ReplSetTest({name: name, nodes: 2});
rst.startSet();
rst.initiate();

// Deliberately set a write concern and read concern that are different from the default w:1 and
// local values.
// The write concern is unsatisfiable, so any operations run in the shutdown hooks will fail if
// they inherit it.
assert.commandWorked(rst.getPrimary().adminCommand({
    setDefaultRWConcern: 1,
    defaultWriteConcern: {w: 42},
    defaultReadConcern: {level: "majority"}
}));

// It should always be possible to successfully stop the replset (including running consistency
// checks) even when the default write concern is unsatisfiable.
rst.stopSet();
})();
