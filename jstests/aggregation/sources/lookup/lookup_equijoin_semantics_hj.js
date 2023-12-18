/**
 * Tests for $lookup with localField/foreignField syntax using hash join algorithm.
 *
 * @tags: [featureFlagSbeFull]
 */
(function() {
"use strict";

load("jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js");  // For runTests.

runTests({
    localColl: db.lookup_arrays_semantics_local_hj,
    foreignColl: db.lookup_arrays_semantics_foreign_hj,
    currentJoinAlgorithm: JoinAlgorithm.HJ
});
})();
