//
// Validate that join optimization does not run into issues for collectionless aggregations.
//
// @tags: [
//   requires_fcv_83,
// ]
//

try {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: true}));
    assert.commandWorked(db.adminCommand({aggregate: 1, pipeline: [{$queryStats: {}}], cursor: {}}));
    assert.commandWorked(db.adminCommand({aggregate: 1, pipeline: [{$documents: [{myDoc: 1}]}], cursor: {}}));
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
}
