/**
 * Tests that collMod fails when an unrecognized field is included in its 'index' option.
 *
 * @tags: [
 *  requires_non_retryable_commands,
 *  requires_fcv_49
 * ]
 */
(function() {

const collName = "collMod_index_invalid_option";

assert.commandWorked(db.getCollection(collName).createIndex({a: 1}, {expireAfterSeconds: 100}));

assert.commandFailedWithCode(db.runCommand({
    collMod: collName,
    index: {keyPattern: {a: 1}, expireAfterSeconds: 200, invalidOption: 1}
}),
                             40415 /* IDL unknown field error */);
})();
