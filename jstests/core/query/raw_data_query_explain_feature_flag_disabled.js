/**
 * Tests that query explains fail with InvalidOptions when the rawData option is provided but the
 * feature flag is not enabled.
 *
 * @tags: [
 *   # Refusing to run a test that issues an aggregation command with explain because it may return
 *   # incomplete results if interrupted by a stepdown.
 *   does_not_support_stepdowns,
 *   featureFlagRawDataCrudOperations_incompatible,
 *   requires_fcv_81,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.throwsWithCode(() => coll.explain().aggregate([{$match: {}}], {rawData: true}),
                      ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.explain().count({key: 1}, {rawData: true}),
                      ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.explain().distinct("key", {}, {rawData: true}),
                      ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.explain().find().rawData().finish(), ErrorCodes.InvalidOptions);
