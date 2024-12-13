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

// Generate a document set with both the 'score' and 'textScore' metadata fields projected as
// regular fields.
let results = coll.aggregate([
                      {$match: {$text: {$search: "test"}}},
                      {
                          $project: {
                              [kTextScoreMetadataArg]: {$meta: kTextScoreMetadataArg},
                              [kScoreMetadataArg]: {$meta: kScoreMetadataArg}
                          }
                      }
                  ])
                  .toArray();

// Ensure both 'score' and 'textScore' are present and equal.
assert.neq(results.length, 0, "results array expected not to be empty");
for (let result of results) {
    assert(result.hasOwnProperty(kTextScoreMetadataArg));
    assert(result.hasOwnProperty(kScoreMetadataArg));
    assert.eq(result[kTextScoreMetadataArg],
              result[kScoreMetadataArg],
              "the legacy metadata value '" + kTextScoreMetadataArg +
                  "' is not equal to the 'score' metadata value");
}
