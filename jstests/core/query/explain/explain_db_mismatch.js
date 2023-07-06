// Ensure that explain command errors if the inner command has a $db field that doesn't match the
// outer command.
assert.commandFailedWithCode(db.runCommand({explain: {find: 'some_collection', $db: 'not_my_db'}}),
                             ErrorCodes.InvalidNamespace);