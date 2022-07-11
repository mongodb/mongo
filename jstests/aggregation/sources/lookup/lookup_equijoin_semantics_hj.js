/**
 * Tests for $lookup with localField/foreignField syntax using hash join algorithm.
 */
(function() {
"use strict";

load("jstests/libs/sbe_util.js");  // For checkSBEEnabled.
load("jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js");  // For runTests.

if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping the test because it only applies to $lookup in SBE");
    return;
}

localColl = db.lookup_arrays_semantics_local_hj;
foreignColl = db.lookup_arrays_semantics_foreign_hj;

currentJoinAlgorithm = JoinAlgorithm.HJ;
runTests();
})();
