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

    // This pipeline's shape would normally be eligible for join-reordeing.
    // Ensure we process it correctly for non-existent collections.
    const baseColl = db[jsTestName()];
    const coll1 = db[jsTestName() + "_a"];
    const coll2 = db[jsTestName() + "_b"];
    const pipeline = [
        {$lookup: {from: jsTestName() + "_a", localField: "a", foreignField: "a", as: "x"}},
        {$unwind: "$x"},
        {$lookup: {from: jsTestName() + "_b", localField: "b", foreignField: "b", as: "y"}},
        {$unwind: "$y"},
    ];

    // None of these collections exist.
    function assertNDocsReturned(n) {
        assert.eq(baseColl.aggregate(pipeline).toArray().length, n);
    }

    // Create the base coll.
    const doc = {a: 1, b: 1};
    assert.commandWorked(baseColl.insertOne(doc));
    assertNDocsReturned(0);

    // Create the second coll.
    assert.commandWorked(coll1.insertOne(doc));
    assertNDocsReturned(0);

    // Create the last coll- now this should return one doc.
    assert.commandWorked(coll2.insertOne(doc));
    assertNDocsReturned(1);
} finally {
    assert.commandWorked(db.adminCommand({setParameter: 1, internalEnableJoinOptimization: false}));
}
