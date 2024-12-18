/**
 * Tests using "searchScore" (and its equivalent alias metadata field "score") in a sort expression.
 * This isn't expected to be very common, but one anticipated use case is to compute rank or other
 * window fields, where a sort expression is required.
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

// Main testing function that runs all sub-tests.
// Input parameter is the name of the meta field that should be sorted on
// (i.e. "searchScore" or "score")
function runTest(metadataSortFieldName) {
    const coll = db.sort_by_search_score;
    coll.drop();
    assert.commandWorked(coll.insert([
        {_id: 0, size: "small"},
        {_id: 1, size: "medium", mood: "content hippo"},
        {_id: 2, size: "large", mood: "very hungry hippo"}
    ]));

    createSearchIndex(coll, {name: "test-dynamic", definition: {"mappings": {"dynamic": true}}});
    var searchIndexes = coll.aggregate([{"$listSearchIndexes": {}}]).toArray();
    assert.eq(searchIndexes.length, 1, searchIndexes);

    const searchForHungryHippo = {
        $search: {
            index: "test-dynamic",
            text: {query: "hungry hippo", path: ["mood"]},
        }
    };

    // Test basic ranking, with no 'partitionBy'.
    {
        const results = coll.aggregate([
                                searchForHungryHippo,
                                {
                                    $setWindowFields: {
                                        sortBy: {score: {$meta: metadataSortFieldName}},
                                        output: {rank: {$rank: {}}}
                                    }
                                },
                            ])
                            .toArray();

        // We should see that the document matching both search terms scores higher than the one
        // matching only a single term.
        assert.eq(results.length, 2);
        assert.docEq(results[0], {_id: 2, size: "large", mood: "very hungry hippo", rank: 1});
        assert.docEq(results[1], {_id: 1, size: "medium", mood: "content hippo", rank: 2});
    }
    {
        // Test ranking with ties.
        const scoreForAnyMatch = 1;
        const results = coll.aggregate([
                                {
                                    $search: {
                                        index: "test-dynamic",
                                        text: {
                                            query: "hungry hippo",
                                            path: ["mood"],
                                            score: {constant: {value: scoreForAnyMatch}}
                                        },
                                    }
                                },
                                {
                                    $setWindowFields: {
                                        sortBy: {score: {$meta: metadataSortFieldName}},
                                        output: {rank: {$rank: {}}}
                                    }
                                },
                                {$sort: {_id: 1}}
                            ])
                            .toArray();

        assert.eq(results.length, 2);
        // Because we manually override the score by passing the 'score' option, these should tie.
        // The order in 'results' is based on the trailing $sort by '_id'.
        assert.docEq(results[0], {_id: 1, size: "medium", mood: "content hippo", rank: 1});
        assert.docEq(results[1], {_id: 2, size: "large", mood: "very hungry hippo", rank: 1});
    }

    {
        // Test with a 'partitionBy' argument.
        const results = coll.aggregate([
                                searchForHungryHippo,
                                {
                                    $setWindowFields: {
                                        sortBy: {score: {$meta: metadataSortFieldName}},
                                        partitionBy: '$size',
                                        output: {rank: {$rank: {}}}
                                    }
                                },
                                {$sort: {_id: 1}},
                            ])
                            .toArray();
        // Now that we have partitioned by size, each document is in first place within their size
        // category. The results are still in a deteministic order due to the sort on _id.
        assert.eq(results.length, 2);
        assert.docEq(results[0], {_id: 1, size: "medium", mood: "content hippo", rank: 1});
        assert.docEq(results[1], {_id: 2, size: "large", mood: "very hungry hippo", rank: 1});
    }

    dropSearchIndex(coll, {name: "test-dynamic"});
}

runTest("searchScore");
runTest("score");
