// Ensure that explain command errors if the inner command has a $db field that doesn't match the
// outer command.
// @tags: [
//  # Test has an inner "$db" in explain find command that can't be overriden by the
//  # `simulate_atlas_proxy` override.
//  simulate_atlas_proxy_incompatible,
// ]
assert.commandFailedWithCode(db.runCommand({explain: {find: 'some_collection', $db: 'not_my_db'}}),
                             ErrorCodes.InvalidNamespace);