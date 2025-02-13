/**
 * Tests that write explains fail with InvalidOptions when the rawData option is provided but the
 * feature flag is not enabled.
 *
 * @tags: [
 *   featureFlagRawDataCrudOperations_incompatible,
 *   requires_fcv_81,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.throwsWithCode(() => coll.explain().update({key: 1}, {key: 2}, {rawData: true}),
                      ErrorCodes.InvalidOptions);
assert.throwsWithCode(() => coll.explain().remove({key: 1}, {rawData: true, justOne: true}),
                      ErrorCodes.InvalidOptions);
