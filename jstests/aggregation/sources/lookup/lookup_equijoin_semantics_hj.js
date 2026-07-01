/**
 * Tests for $lookup with localField/foreignField syntax using hash join algorithm.
 *
 * @tags: [
 *   featureFlagSbeFull,
 *   # SERVER-36681 changed the behavior of SBE and classic engines
 *   requires_fcv_90,
 * ]
 */
import {
    JoinAlgorithm,
    runTests,
} from "jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js";

runTests({
    localColl: db.lookup_arrays_semantics_local_hj,
    foreignColl: db.lookup_arrays_semantics_foreign_hj,
    currentJoinAlgorithm: JoinAlgorithm.HJ,
});
