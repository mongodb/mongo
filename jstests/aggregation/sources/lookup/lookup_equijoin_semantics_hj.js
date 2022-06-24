/**
 * Tests for $lookup with localField/foreignField syntax using hash join algorithm.
 */
(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.
load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.
load("jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js");  // For runTests.

if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping the test because it only applies to $lookup in SBE");
    return;
}

localColl = db.lookup_arrays_semantics_local_hj;
foreignColl = db.lookup_arrays_semantics_foreign_hj;

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(foreignColl) && !isShardedLookupEnabled) {
    return;
}

currentJoinAlgorithm = JoinAlgorithm.HJ;
runTests();
})();
