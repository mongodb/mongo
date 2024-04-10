/**
 * Tests that collMod fails when an unrecognized field is included in its 'index' option.
 *
 * @tags: [
 *   requires_non_retryable_commands,
 * ]
 */
const collName = "collMod_index_invalid_option";

assert.commandWorked(db.getCollection(collName).createIndex({a: 1}, {expireAfterSeconds: 100}));

assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, expireAfterSeconds: 200, invalidOption: 1}
}),
                             ErrorCodes.IDLUnknownField);
