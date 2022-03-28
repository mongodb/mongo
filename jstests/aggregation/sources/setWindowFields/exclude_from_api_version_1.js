/**
 * Test that $setWindowFields is excluded from API version 1.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();
coll.insert({a: 1});
coll.insert({a: 2});

// Test that $count is included from API Version 1 so long as it's used in $group.
assert.commandWorked(db.runCommand({
    aggregate: coll.getName(),
    pipeline: [{$group: {_id: null, count: {$count: {}}}}],
    cursor: {},
    apiVersion: "1",
    apiStrict: true
}));
})();
