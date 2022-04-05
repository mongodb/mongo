/**
 * Tests for $lookup with localField/foreignField syntax using nested loop join algorithm.
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");                                      // For isSharded.
load("jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js");  // For runTests.

localColl = db.lookup_arrays_semantics_local_nlj;
foreignColl = db.lookup_arrays_semantics_foreign_nlj;

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(foreignColl) && !isShardedLookupEnabled) {
    return;
}

currentJoinAlgorithm = JoinAlgorithm.NLJ;
runTests();
})();
