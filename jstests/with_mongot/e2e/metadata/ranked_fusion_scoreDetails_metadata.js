/**
 * Tests that the 'scoreDetails' metadata field is appropriately accessible or inaccessible from a
 * $rankFusion stage. This test focuses on exercising scoreDetails dependency analysis in various
 * pipeline structures and does not verify correctness of the scoreDetails field contents itself.
 *
 * @tags: [ featureFlagRankFusionFull, requires_fcv_81 ]
 */

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

/**
 * Run a $rankFusion pipeline followed by a {$meta: "scoreDetails"} projection and validate whether
 * or not score details are present in the results.
 *
 * @param {boolean} requestScoreDetails request $rankFusion to generate score details metadata.
 * @param {boolean} requestSearchScoreDetails request the $search nested inside $rankFusion to
 *     generate score details. Defaults to false.
 * @param {boolean} forceProjectionOnMerger force the metadata project to be run on the merging
 *     shard, otherwise allow it to be pushed down. Defaults to false (i.e. no pipeline split).
 * @param {int} numInputPipelines the number of input pipelines to pass to $rankFusion. Defaults
 *     to 1.
 */
function runTest({
    requestScoreDetails,
    requestSearchScoreDetails = false,
    forceProjectionOnMerger = false,
    numInputPipelines = 1
}) {
    const searchQueryWithScoreDetails =
        Object.assign({scoreDetails: requestSearchScoreDetails}, searchQuery);

    let inputPipelines = {};
    for (let i = 0; i < numInputPipelines; ++i) {
        inputPipelines[`searchPipeline${i}`] = [{$search: searchQueryWithScoreDetails}];
    }

    let pipeline = [
        {
            $rankFusion: {input: {pipelines: inputPipelines}, scoreDetails: requestScoreDetails},
        },
    ];

    if (forceProjectionOnMerger) {
        // Add a splitPipeline stage so that the $project will stay on the merging shard.
        pipeline.push({$_internalSplitPipeline: {}});
    }

    pipeline.push({$project: scoreDetailsProjection});

    const results = coll.aggregate(pipeline).toArray();
    assert.eq(results.length, 2);

    for (let result of results) {
        if (requestScoreDetails) {
            assert(result.hasOwnProperty("scoreDetails"),
                   `Requested scoreDetails to be calculated, but was not found on document ${
                       tojson(result)}`);
            assert(result["scoreDetails"].hasOwnProperty("details"),
                   `scoreDetails had unexpected contents in document ${tojson(result)}`);

            for (let i = 0; i < numInputPipelines; ++i) {
                assert(result["scoreDetails"]["details"].hasOwnProperty(`searchPipeline${i}`),
                       `scoreDetails had unexpected contents in document ${tojson(result)}`);
            }
        } else {
            // TODO SERVER-99169 Throw an error instead of returning empty results.
            assert(!result.hasOwnProperty("scoreDetails"),
                   `Did not request scoreDetails to be calculated, but was found on document ${
                       tojson(result)}`);
        }
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

// Request $searchScoreDetails and ensure that it is not directly accessible outside of $rankFusion.
runTest({requestScoreDetails: true, requestSearchScoreDetails: true});
runTest({requestScoreDetails: true, requestSearchScoreDetails: true, numInputPipelines: 2});
// TODO SERVER-99169 Nested $search's scoreDetails probably should not be accessible outside of the
// $rankFusion pipeline even when we don't request $rankFusion scoreDetails.

// runTest({requestScoreDetails: false, requestSearchScoreDetails: true });

// runTest({ requestScoreDetails: false, requestSearchScoreDetails: true, numInputPipelines: 2 });

{
    // Run $rankFusion in a $unionWith.
    const results =
        coll.aggregate([
                {
                    $unionWith: {
                        coll: collName,
                        pipeline: [
                            {
                                $rankFusion: {
                                    input: {pipelines: {search: [{$search: searchQuery}]}},
                                    scoreDetails: true
                                },
                            },
                        ]
                    }
                },
                {$project: scoreDetailsProjection},
            ])
            .toArray();
    assert.eq(results.length, 6);

    // TODO SERVER-99169 Determine what should happen here (currently some documents have
    // scoreDetails and some don't).
}

{
    // Run $rankFusion in a $lookup.
    const results = coll.aggregate([
        {
            $lookup: {
                from: collName,
                as: "docs",
                pipeline: [{
                    $rankFusion: {
                        input: {
                            pipelines: {search: [{$search: searchQuery}]}
                        },
                        scoreDetails: true
                    },
                },]
            }
        },
        { $project: scoreDetailsProjection },
    ]).toArray();
    assert.eq(results.length, 4);

    for (let result of results) {
        // TODO SERVER-99169 Throw an error instead of returning empty results.
        assert(!result.hasOwnProperty("scoreDetails"),
               `Did not request scoreDetails to be calculated, but was found on document ${
                   tojson(result)}`);
    }
}

dropSearchIndex(coll, {name: indexName});
