/**
 * Tests that writes fail with InvalidOptions when the rawData option is provided but the feature
 * flag is not enabled.
 *
 * @tags: [
 *   featureFlagRawDataCrudOperations_incompatible,
 *   requires_fcv_81,
 * ]
 */

const coll = db[jsTestName()];
coll.drop();

assert.commandFailedWithCode(coll.insert({key: 1}, {rawData: true}), ErrorCodes.InvalidOptions);
// TODO (SERVER-97452, SERVER-97453): Tests for update and delete with rawData option.
