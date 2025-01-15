/**
 * Tests that the "text score" metadata (previously known as 'textScore') is accessible by the
 * 'score' metadata field.
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */

const kScoreMetadataArg = "score";
const kTextScoreMetadataArg = "textScore";

const coll = db.foo;
coll.drop();

const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < 10; ++i) {
    bulk.insert({_id: i, x: "test"});
}
assert.commandWorked(bulk.execute());

assert.commandWorked(coll.createIndex({x: "text"}));

function runTest({forceProjectionOnMerger}) {
    function buildPipeline(projections) {
        let pipeline = [{$match: {$text: {$search: "test"}}}];

        if (forceProjectionOnMerger) {
            // Add a splitPipeline stage so that the $project will stay on the merging shard and
            // exercise dependency analysis there.
            pipeline.push({$_internalSplitPipeline: {}});
        }

        pipeline.push({$project: projections});

        return pipeline;
    }

    const pipelineWithScoreAndTextScore = buildPipeline({
        [kTextScoreMetadataArg]: {$meta: kTextScoreMetadataArg},
        [kScoreMetadataArg]: {$meta: kScoreMetadataArg}
    });

    // Generate a document set with both the 'score' and 'textScore' metadata fields projected as
    // regular fields.
    let results = coll.aggregate(pipelineWithScoreAndTextScore).toArray();

    // Ensure both 'score' and 'textScore' are present and equal.
    assert.neq(results.length, 0, "results array expected not to be empty");
    for (let result of results) {
        assert(result.hasOwnProperty(kTextScoreMetadataArg),
               `Failed to find ${kTextScoreMetadataArg} in document: ${tojson(result)}`);
        assert(result.hasOwnProperty(kScoreMetadataArg),
               `Failed to find ${kScoreMetadataArg} in document: ${tojson(result)}`);
        assert.eq(result[kTextScoreMetadataArg],
                  result[kScoreMetadataArg],
                  "the legacy metadata value '" + kTextScoreMetadataArg +
                      "' is not equal to the 'score' metadata value");
    }

    // We should also be able to project $score by itself.
    const pipelineWithScoreOnly = buildPipeline({[kScoreMetadataArg]: {$meta: kScoreMetadataArg}});

    results = coll.aggregate(pipelineWithScoreOnly).toArray();
    assert.neq(results.length, 0, "results array expected not to be empty");

    // TODO SERVER-99335 Enable this validation once score is populated correctly for this case.
    // for (let result of results) {
    //     assert(result.hasOwnProperty(kScoreMetadataArg));
    // }
}

runTest({forceProjectionOnMerger: false});
runTest({forceProjectionOnMerger: true});
