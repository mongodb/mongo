/**
 * Test that trying to use $_resumeAfter and $_requestResumeToken options to resume a query fails
 * on mongos.
 * @tags: [multiversion_incompatible,requires_find_command]
 */

(function() {
"use strict";

const st = new ShardingTest({shards: 1});
const db = st.getDB("test");

// $_requestResumeToken is disallowed.
assert.commandFailedWithCode(
    db.runCommand({find: "test", hint: {$natural: 1}, batchSize: 1, $_requestResumeToken: true}),
    ErrorCodes.BadValue);

// Passing a $_resumeAfter token is disallowed.
assert.commandFailedWithCode(db.runCommand({
    find: "test",
    hint: {$natural: 1},
    batchSize: 1,
    $_resumeAfter: {$recordId: NumberLong(1)}
}),
                             ErrorCodes.BadValue);

// Passing both is disallowed.
assert.commandFailedWithCode(db.runCommand({
    find: "test",
    hint: {$natural: 1},
    batchSize: 1,
    $_requestResumeToken: true,
    $_resumeAfter: {$recordId: NumberLong(1)}
}),
                             ErrorCodes.BadValue);

st.stop();
})();
