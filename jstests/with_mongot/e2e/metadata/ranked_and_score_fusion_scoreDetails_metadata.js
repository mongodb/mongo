/**
 * Tests that the 'scoreDetails' metadata field is appropriately accessible or inaccessible from a
 * $rankFusion or $scoreFusion stage. This test focuses on exercising scoreDetails dependency
 * analysis in various pipeline structures and does not verify correctness of the scoreDetails field
 * contents itself.
 *
 * @tags: [ featureFlagRankFusionFull, featureFlagSearchHybridScoringFull, requires_fcv_81 ]
 */

import {assertErrCodeAndErrMsgContains} from "jstests/aggregation/extras/utils.js";
import {createSearchIndex, dropSearchIndex} from "jstests/libs/search.js";

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insertMany([
    {floor: 0, storeName: "Bob's Burger"},
    {floor: 1, storeName: "Burgers Galore"},
    {floor: 2, storeName: "Hot Dog Stand"},
    {floor: 3, storeName: "Burger Wanna Burger"}
]));

const indexName = "mall_stores_index";
createSearchIndex(coll, {name: indexName, definition: {"mappings": {"dynamic": true}}});

const searchQuery = {
    index: indexName,
    text: {query: "Burger", path: ["storeName"]},
};

const scoreDetailsProjection = {
    scoreDetails: {$meta: "scoreDetails"}
};

function assertFailsScoreDetailsUnavailable(pipeline) {
    const kUnavailableMetadataErrCode = 40218;
    assertErrCodeAndErrMsgContains(
        coll, pipeline, kUnavailableMetadataErrCode, "query requires scoreDetails metadata");
}

function assertCorrectScoreDetailsStructure(resultDoc, numInputPipelines) {
    assert(resultDoc.hasOwnProperty("scoreDetails"),
           `Requested scoreDetails to be calculated, but was not found on document ${
               tojson(resultDoc)}`);
    assert(resultDoc["scoreDetails"].hasOwnProperty("details"),
           `scoreDetails had unexpected contents in document ${tojson(resultDoc)}`);

    assert.eq(resultDoc["scoreDetails"]["details"].length,
              numInputPipelines,
              `scoreDetails had unexpected contents in document ${tojson(resultDoc)}`);

    for (let i = 0; i < numInputPipelines; ++i) {
        assert.eq(resultDoc["scoreDetails"]["details"][i]["inputPipelineName"],
                  `searchPipeline${i}`,
                  `scoreDetails had unexpected contents in document ${tojson(resultDoc)}`);
    }
}

/**
 * Run a $rankFusion and $scoreFusion pipeline followed by a {$meta: "scoreDetails"} projection and
 * validate whether or not score details are present in the results.
 *
 * @param {boolean} requestScoreDetails request $rankFusion and $scoreFusion to generate score
 *     details metadata.
 * @param {boolean} requestSearchScoreDetails request the $search nested inside $rankFusion and
 *     $scoreFusion to generate score details. Defaults to false.
 * @param {boolean} forceProjectionOnMerger force the metadata project to be run on the merging
 *     shard, otherwise allow it to be pushed down. Defaults to false (i.e. no pipeline split).
 * @param {int} numInputPipelines the number of input pipelines to pass to $rankFusion and
 *     $scoreFusion. Defaults to 1.
 */
function runTest({
    requestScoreDetails,
    requestSearchScoreDetails = false,
    forceProjectionOnMerger = false,
    numInputPipelines = 1,
}) {
    const searchQueryWithScoreDetails =
        Object.assign({scoreDetails: requestSearchScoreDetails}, searchQuery);

    let inputPipelines = {};
    for (let i = 0; i < numInputPipelines; ++i) {
        inputPipelines[`searchPipeline${i}`] = [{$search: searchQueryWithScoreDetails}];
    }

    const rankFusionPipeline = [
        {
            $rankFusion: {input: {pipelines: inputPipelines}, scoreDetails: requestScoreDetails},
        },
    ];

    const scoreFusionPipeline = [
        {
            $scoreFusion: {
                input: {pipelines: inputPipelines, normalization: "none"},
                scoreDetails: requestScoreDetails
            },
        },
    ];

    if (forceProjectionOnMerger) {
        // Add a splitPipeline stage so that the $project will stay on the merging shard.
        rankFusionPipeline.push({$_internalSplitPipeline: {}});
        scoreFusionPipeline.push({$_internalSplitPipeline: {}});
    }

    rankFusionPipeline.push({$project: scoreDetailsProjection});
    scoreFusionPipeline.push({$project: scoreDetailsProjection});

    if (requestScoreDetails) {
        const rankFusionResults = coll.aggregate(rankFusionPipeline).toArray();
        assert.eq(rankFusionResults.length, 2);
        for (let result of rankFusionResults) {
            assertCorrectScoreDetailsStructure(result, numInputPipelines);
        }

        const scoreFusionResults = coll.aggregate(scoreFusionPipeline).toArray();
        assert.eq(scoreFusionResults.length, 2);
        for (let result of scoreFusionResults) {
            assertCorrectScoreDetailsStructure(result, numInputPipelines);
        }
    } else {
        assertFailsScoreDetailsUnavailable(rankFusionPipeline);
        assertFailsScoreDetailsUnavailable(scoreFusionPipeline);
    }
}

// Basic tests - are $scoreDetails present when we ask for them?
runTest({requestScoreDetails: true});
runTest({requestScoreDetails: true, numInputPipelines: 2});
runTest({requestScoreDetails: false});
runTest({requestScoreDetails: false, numInputPipelines: 2});

// Force the $scoreDetails projection to stay on the merging shard.
runTest({requestScoreDetails: true, forceProjectionOnMerger: true});
runTest({requestScoreDetails: false, forceProjectionOnMerger: true});

// Request $searchScoreDetails and ensure that it is not directly accessible outside of $rankFusion
// and $scoreFusion.
runTest({requestScoreDetails: true, requestSearchScoreDetails: true});
runTest({requestScoreDetails: true, requestSearchScoreDetails: true, numInputPipelines: 2});
runTest({
    requestScoreDetails: true,
    requestSearchScoreDetails: true,
    numInputPipelines: 2,
    forceProjectionOnMerger: true
});
runTest({requestScoreDetails: false, requestSearchScoreDetails: true});
runTest({requestScoreDetails: false, requestSearchScoreDetails: true, numInputPipelines: 2});
runTest({
    requestScoreDetails: false,
    requestSearchScoreDetails: true,
    numInputPipelines: 2,
    forceProjectionOnMerger: true
});

// Run $rankFusion in a $unionWith and request "scoreDetails" from the outer pipeline. This
// should fail since "scoreDetails" is not generated by the outer (empty) pipeline.
{
    const pipeline = [
        {
            $unionWith: {
                coll: collName,
                pipeline: [
                    {
                        $rankFusion: {
                            input: {pipelines: {searchPipeline0: [{$search: searchQuery}]}},
                            scoreDetails: true
                        },
                    },
                ]
            }
        },
        {$project: scoreDetailsProjection},
    ];
    assertFailsScoreDetailsUnavailable(pipeline);
}

// Run $scoreFusion in a $unionWith and request "scoreDetails" from the outer pipeline. This
// should fail since "scoreDetails" is not generated by the outer (empty) pipeline.
{
    const pipeline = [
        {
            $unionWith: {
                coll: collName,
                pipeline: [
                    {
                        $scoreFusion: {
                            input: {
                                pipelines: {searchPipeline0: [{$search: searchQuery}]},
                                normalization: "none"
                            },
                            scoreDetails: true
                        },
                    },
                ]
            }
        },
        {$project: scoreDetailsProjection},
    ];
    assertFailsScoreDetailsUnavailable(pipeline);
}

// Run $rankFusion in a $unionWith and request "scoreDetails" from the inner pipeline. This
// should succeed since "scoreDetails" is generated by the inner pipeline.
{
    const results =
        coll.aggregate([
                {
                    $unionWith: {
                        coll: collName,
                        pipeline: [
                            {
                                $rankFusion: {
                                    input: {pipelines: {searchPipeline0: [{$search: searchQuery}]}},
                                    scoreDetails: true
                                },
                            },
                            {$project: scoreDetailsProjection},
                        ]
                    }
                },
            ])
            .toArray();
    assert.eq(results.length, 6);

    // The first 4 documents from the outer pipeline should not have scoreDetails.
    for (let i = 0; i < 4; i++) {
        assert(!results[i].hasOwnProperty("scoreDetails"));
    }

    // The final 2 documents from the subpipeline should have scoreDetails.
    for (let i = 4; i < 6; i++) {
        const numInputPipelines = 1;
        assertCorrectScoreDetailsStructure(results[i], numInputPipelines);
    }
}

// Run $scoreFusion in a $unionWith and request "scoreDetails" from the inner pipeline. This
// should succeed since "scoreDetails" is generated by the inner pipeline.
{
    const results =
        coll.aggregate([
                {
                    $unionWith: {
                        coll: collName,
                        pipeline: [
                            {
                                $scoreFusion: {
                                    input: {
                                        pipelines: {searchPipeline0: [{$search: searchQuery}]},
                                        normalization: "none"
                                    },
                                    scoreDetails: true
                                },
                            },
                            {$project: scoreDetailsProjection},
                        ]
                    }
                },
            ])
            .toArray();
    assert.eq(results.length, 6);

    // The first 4 documents from the outer pipeline should not have scoreDetails.
    for (let i = 0; i < 4; i++) {
        assert(!results[i].hasOwnProperty("scoreDetails"));
    }

    // The final 2 documents from the subpipeline should have scoreDetails.
    for (let i = 4; i < 6; i++) {
        const numInputPipelines = 1;
        assertCorrectScoreDetailsStructure(results[i], numInputPipelines);
    }
}

// Run $rankFusion in a $lookup and request "scoreDetails" from the outer pipeline. This
// should fail since "scoreDetails" is not generated by the outer (empty) pipeline.
{
    const pipeline = [
        {
            $lookup: {
                from: collName,
                as: "docs",
                pipeline: [{
                    $rankFusion: {
                        input: {
                            pipelines: {searchPipeline0: [{$search: searchQuery}]}
                        },
                        scoreDetails: true
                    },
                },]
            }
        },
        { $project: scoreDetailsProjection },
    ];
    assertFailsScoreDetailsUnavailable(pipeline);
}

// Run $scoreFusion in a $lookup and request "scoreDetails" from the outer pipeline. This
// should fail since "scoreDetails" is not generated by the outer (empty) pipeline.
{
    const pipeline = [
        {
            $lookup: {
                from: collName,
                as: "docs",
                pipeline: [{
                    $scoreFusion: {
                        input: {
                            pipelines: {searchPipeline0: [{$search: searchQuery}]},
                            normalization: "none"
                        },
                        scoreDetails: true
                    },
                },]
            }
        },
        { $project: scoreDetailsProjection },
    ];
    assertFailsScoreDetailsUnavailable(pipeline);
}

// Run $rankFusion in a $lookup and request "scoreDetails" from the inner pipeline. This
// should succeed since "scoreDetails" is generated by the inner pipeline.
{
    const results = coll.aggregate([
        {
            $lookup: {
                from: collName,
                as: "docs",
                pipeline: [{
                    $rankFusion: {
                        input: {
                            pipelines: {searchPipeline0: [{$search: searchQuery}]}
                        },
                        scoreDetails: true
                    },
                }, { $project: scoreDetailsProjection },
                ]
            }
        },
        ])
        .toArray();
    assert.eq(results.length, 4);

    for (let result of results) {
        // The top-level document should not have scoreDetails.
        assert(!result.hasOwnProperty("scoreDetails"),
               `Did not request scoreDetails to be calculated, but was found on document ${
                   tojson(result)}`);

        // The subpipeline-generate data should include scoreDetails.
        let docs = result["docs"];
        assert.eq(docs.length, 2);
        for (let subpipelineDoc of docs) {
            const numInputPipelines = 1;
            assertCorrectScoreDetailsStructure(subpipelineDoc, numInputPipelines);
        }
    }
}

// Run $scoreFusion in a $lookup and request "scoreDetails" from the inner pipeline. This
// should succeed since "scoreDetails" is generated by the inner pipeline.
{
    const results = coll.aggregate([
        {
            $lookup: {
                from: collName,
                as: "docs",
                pipeline: [{
                    $scoreFusion: {
                        input: {
                            pipelines: {searchPipeline0: [{$search: searchQuery}]},
                            normalization: "none"
                        },
                        scoreDetails: true
                    },
                }, { $project: scoreDetailsProjection },
                ]
            }
        },
        ])
        .toArray();
    assert.eq(results.length, 4);

    for (let result of results) {
        // The top-level document should not have scoreDetails.
        assert(!result.hasOwnProperty("scoreDetails"),
               `Did not request scoreDetails to be calculated, but was found on document ${
                   tojson(result)}`);

        // The subpipeline-generate data should include scoreDetails.
        let docs = result["docs"];
        assert.eq(docs.length, 2);
        for (let subpipelineDoc of docs) {
            const numInputPipelines = 1;
            assertCorrectScoreDetailsStructure(subpipelineDoc, numInputPipelines);
        }
    }
}

// Verify that scoreDetails cannot be projected from a $facet subpipeline where $rankFusion sets
// scoreDetails to false.
{
    const pipeline = [
        {
            $rankFusion:
                {input: {pipelines: {pipe: [{$sort: {textField: -1}}]}}, scoreDetails: false}
        },
        {
            $facet: {
                pipe1: [
                    {$project: {scoreDetails: {$meta: "scoreDetails"}}},
                    {$project: {scoreDetailsVal: "$scoreDetails.value"}},
                    {$sort: {_id: 1}}
                ]
            }
        }
    ];
    assertFailsScoreDetailsUnavailable(pipeline);
}

// Verify that scoreDetails cannot be projected from a $facet subpipeline where $scoreFusion sets
// scoreDetails to false.
{
    const pipeline = [
        {
            $scoreFusion: {
                input: {pipelines: {pipe: [{$search: searchQuery}]}, normalization: "none"},
                scoreDetails: false
            }
        },
        {
            $facet: {
                pipe1: [
                    {$project: {scoreDetails: {$meta: "scoreDetails"}}},
                    {$project: {scoreDetailsVal: "$scoreDetails.value"}},
                    {$sort: {_id: 1}}
                ]
            }
        }
    ];
    assertFailsScoreDetailsUnavailable(pipeline);
}

// Verify that scoreDetails can be projected from a $facet subpipeline where $rankFusion sets
// scoreDetails to true.
{
    const results =
        coll.aggregate([
                {
                    $rankFusion:
                        {input: {pipelines: {pipe: [{$sort: {textField: -1}}]}}, scoreDetails: true}
                },
                {
                    $facet: {
                        pipe1: [
                            {$project: {scoreDetails: {$meta: "scoreDetails"}}},
                            {
                                $project: {
                                    scoreDetailsVal: "$scoreDetails.value",
                                    score: {$meta: "score"}
                                }
                            },
                            {$sort: {_id: 1}}
                        ]
                    }
                }
            ])
            .toArray();

    for (let result of results) {
        let docs = result["pipe1"];
        for (let doc of docs) {
            assert(doc.hasOwnProperty("scoreDetailsVal"),
                   `Result doc does not have property
            'scoreDetailsVal'. See  ${tojson(doc)}`);
            assert(doc.hasOwnProperty("score"),
                   `Result doc does not have property
            'score'. See  ${tojson(doc)}`);
        }
    }
}

// Verify that scoreDetails can be projected from a $facet subpipeline where $scoreFusion sets
// scoreDetails to true.
{
    const results =
        coll.aggregate([
                {
                    $scoreFusion: {
                        input: {pipelines: {pipe: [{$search: searchQuery}]}, normalization: "none"},
                        scoreDetails: true
                    }
                },
                {
                    $facet: {
                        pipe1: [
                            {$project: {scoreDetails: {$meta: "scoreDetails"}}},
                            {
                                $project: {
                                    scoreDetailsVal: "$scoreDetails.value",
                                    score: {$meta: "score"}
                                }
                            },
                            {$sort: {_id: 1}}
                        ]
                    }
                }
            ])
            .toArray();

    for (let result of results) {
        let docs = result["pipe1"];
        for (let doc of docs) {
            assert(doc.hasOwnProperty("scoreDetailsVal"),
                   `Result doc does not have property
            'scoreDetailsVal'. See  ${tojson(doc)}`);
            assert(doc.hasOwnProperty("score"),
                   `Result doc does not have property
            'score'. See  ${tojson(doc)}`);
        }
    }
}
dropSearchIndex(coll, {name: indexName});
