/**
 * Tests for $lookup with localField/foreignField syntax using nested loop join algorithm.
 */
(function() {
"use strict";

load("jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js");  // For runTests.

localColl = db.lookup_arrays_semantics_local_nlj;
foreignColl = db.lookup_arrays_semantics_foreign_nlj;

currentJoinAlgorithm = JoinAlgorithm.NLJ;
runTests();
})();
