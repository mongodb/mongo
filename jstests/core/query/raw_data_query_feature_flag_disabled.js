/**
 * Tests that queries fail with InvalidOptions when the rawData option is provided but the feature
 * flag is not enabled.
 *
 * @tags: [
 *   featureFlagRawDataCrudOperations_incompatible,
 *   requires_fcv_81,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.throwsWithCode(() => coll.aggregate([{$match: {}}], {rawData: true}),
                      ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.count({key: 1}, {rawData: true}), ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.distinct("key", {}, {rawData: true}), ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.find().rawData().toArray(), ErrorCodes.InvalidOptions);
