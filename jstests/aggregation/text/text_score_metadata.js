/**
 * Tests that the "text score" metadata (previously known as 'textScore') is accessible by the
 * 'score' metadata field in aggregation pipelines.
 *
 * @tags: [featureFlagRankFusionFull, requires_fcv_81]
 */

const kScoreMetadataArg = "score";
const kTextScoreMetadataArg = "textScore";

const coll = db.foo;
coll.drop();

assert.commandWorked(coll.insertMany([
    {_id: 0, x: "test"},
    {_id: 1, x: "hello"},
    {_id: 2, x: "test test"},
]));

assert.commandWorked(coll.createIndex({x: "text"}));

const textMatchStage = {
    $match: {$text: {$search: "test"}}
};

/**
 * Helper function to project $textScore in an aggregation, including the option to force the
 * projection on the merging shard.
 * @param {boolean} forceProjectionOnMerger force the metadata project to be run on the merging
 *     shard, otherwise allow it to be pushed down.
 */
function runProjectionTest({forceProjectionOnMerger}) {
    function buildPipeline(projections) {
        let pipeline = [textMatchStage];

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

    for (let result of results) {
        assert(result.hasOwnProperty(kScoreMetadataArg), result);
    }
}

(function testProjectScore() {
    runProjectionTest({forceProjectionOnMerger: false});
})();

(function testProjectScoreOnMergingShard() {
    runProjectionTest({forceProjectionOnMerger: true});
})();

(function testSortOnScore() {
    const results =
        coll.aggregate([textMatchStage, {$sort: {[kScoreMetadataArg]: {$meta: kScoreMetadataArg}}}])
            .toArray();
    assert.eq(results, [{_id: 2, x: "test test"}, {_id: 0, x: "test"}]);
})();

(function testGroupOnScore() {
    // We should have two distinct entries for two different returned scores.
    const results =
        coll.aggregate(
                [textMatchStage, {$group: {_id: {$meta: kScoreMetadataArg}, count: {$sum: 1}}}])
            .toArray();
    assert.eq(results.length, 2, results);
})();
