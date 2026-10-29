/**
 * Runs the $lookup localField/foreignField equijoin semantics suite against a single-path
 * WILDCARD foreign index.
 *
 * A wildcard index is sparse-like: it omits documents that are missing the indexed path, so SBE
 * cannot blindly seek it for null/missing local keys. Like the sparse case, it uses the dynamic
 * indexed loop join (DILJ), falling back to a collection scan for null/missing values. This test
 * exercises the full matrix of null/missing/array/nested-path semantics against that path.
 *
 * Hash join is disabled for the duration of the test so that DILJ is deterministically selected.
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
        localColl: db.lookup_equijoin_semantics_local_wildcard,
        foreignColl: db.lookup_equijoin_semantics_foreign_wildcard,
        currentJoinAlgorithm: JoinAlgorithm.DILJ_Wildcard,
    });
} finally {
    assert.commandWorked(
        db.adminCommand({setParameter: 1, [kDisableHashJoinKnob]: originalKnobValue}),
    );
}
