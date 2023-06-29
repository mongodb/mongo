/**
 * Tests for $lookup with localField/foreignField syntax using hash join algorithm.
 */
import {
    JoinAlgorithm,
    runTests
} from "jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js";
import {checkSBEEnabled} from "jstests/libs/sbe_util.js";

if (!checkSBEEnabled(db)) {
    jsTestLog("Skipping the test because it only applies to $lookup in SBE");
    quit();
}

runTests({
    localColl: db.lookup_arrays_semantics_local_hj,
    foreignColl: db.lookup_arrays_semantics_foreign_hj,
    currentJoinAlgorithm: JoinAlgorithm.HJ
});
