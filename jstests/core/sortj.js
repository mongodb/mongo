// Test an in memory sort memory assertion after a plan has "taken over" in the query optimizer
// cursor.
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For FixtureHelpers.

const t = db.jstests_sortj;
t.drop();

t.ensureIndex({a: 1});

const numShards = FixtureHelpers.numberOfShardsForCollection(t);

const big = new Array(100000).toString();
for (let i = 0; i < 1200 * numShards; ++i) {
    t.save({a: 1, b: big});
}

assert.throws(function() {
    t.find({a: {$gte: 0}, c: null}).sort({d: 1}).itcount();
});
t.drop();
})();
