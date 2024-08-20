/**
 * Tests using "searchScore" in a sort expression. This isn't expected to be very common, but one
 * anticipated use case is to compute rank or other window fields, where a sort expression is
 * required.
 * @tags: [featureFlagSearchHybridScoringPrerequisites]
 */
const coll = db.sort_by_search_score;
coll.drop();
assert.commandWorked(coll.insert([
    {_id: 0, size: "small"},
    {_id: 1, size: "medium", mood: "hungry"},
    {_id: 2, size: "medium", mood: "very hungry"},
    {_id: 3, size: "large", mood: "hungry"}
]));

coll.createSearchIndex({name: "test-dynamic", definition: {"mappings": {"dynamic": true}}});
var searchIndexes = coll.aggregate([{"$listSearchIndexes": {}}]).toArray();
assert.eq(searchIndexes.length, 1, searchIndexes);

const searchForHunger = {
    $search: {
        index: "test-dynamic",
        text: {query: "hungry", path: ["mood"]},
    }
};

{
    // Test a basic ranking, with no 'partitionBy'.
    const results =
        coll.aggregate([
                searchForHunger,
                {
                    $setWindowFields:
                        {sortBy: {score: {$meta: "searchScore"}}, output: {rank: {$rank: {}}}}
                },
                {$sort: {score: {$meta: "searchScore"}, size: 1}},
            ])
            .toArray();

    // We should see a tie for first place rank, and then the "very hungry" document in third place.
    // Note there is a tie-breaking sort on 'size' (after assigning rank), so the order here is
    // important/deterministic.
    assert.eq(results.length, 3);
    assert.docEq(results[0], {_id: 3, size: "large", mood: "hungry", rank: 1});
    assert.docEq(results[1], {_id: 1, size: "medium", mood: "hungry", rank: 1});
    assert.docEq(results[2], {_id: 2, size: "medium", mood: "very hungry", rank: 3});
}

{
    // Test with a 'partitionBy' argument.
    const results = coll.aggregate([
                            searchForHunger,
                            {
                                $setWindowFields: {
                                    sortBy: {score: {$meta: "searchScore"}},
                                    partitionBy: '$size',
                                    output: {rank: {$rank: {}}}
                                }
                            },
                            {$sort: {score: {$meta: "searchScore"}, size: 1}},
                        ])
                        .toArray();
    // Now that we have partitioned by size, the same first place documents are still first place,
    // but now the 'very hungry' result is in second place - not third. Note there is a tie-breaking
    // sort on 'size' (after assigning rank), so the order here is important/deterministic.
    assert.eq(results.length, 3);
    assert.docEq(results[0], {_id: 3, size: "large", mood: "hungry", rank: 1});
    assert.docEq(results[1], {_id: 1, size: "medium", mood: "hungry", rank: 1});
    assert.docEq(results[2], {_id: 2, size: "medium", mood: "very hungry", rank: 2});
}
