/**
 * Runs the $lookup localField/foreignField equijoin semantics suite against a SPARSE foreign index.
 *
 * A sparse index omits documents that are missing the indexed field, so SBE cannot blindly seek it
 * for null/missing local keys. Instead it uses the dynamic indexed loop join (DILJ): per local key
 * it seeks the sparse index for real, non-null values and falls back to a collection scan for
 * null/missing values. This test exercises the full matrix of null/missing/array/nested-path
 * semantics against that path.
 *
 * Hash join is disabled for the duration of the test so that DILJ is deterministically selected for the sparse index.
 *
 * @tags: [requires_sbe]
 */
import {
    JoinAlgorithm,
    runTests,
} from "jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js";

const kDisableHashJoinKnob = "internalQueryDisableLookupExecutionUsingHashJoin";
const originalKnobValue = assert.commandWorked(
    db.adminCommand({getParameter: 1, [kDisableHashJoinKnob]: 1}),
)[kDisableHashJoinKnob];
assert.commandWorked(db.adminCommand({setParameter: 1, [kDisableHashJoinKnob]: true}));

try {
    runTests({
        localColl: db.lookup_equijoin_semantics_local_sparse,
        foreignColl: db.lookup_equijoin_semantics_foreign_sparse,
        currentJoinAlgorithm: JoinAlgorithm.DILJ_Asc,
    });
} finally {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, [kDisableHashJoinKnob]: originalKnobValue}),
    );
}
