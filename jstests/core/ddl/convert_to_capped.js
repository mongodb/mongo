/**
 * Test the convertToCapped cmd.
 *
 * @tags: [
 *   # The test runs commands that are not allowed with security token: convertToCapped.
 *   not_allowed_with_signed_security_token,
 *   requires_non_retryable_commands,
 *   requires_capped,
 *   # SERVER-85772 enable testing with balancer once convertToCapped supported on arbitrary shards
 *   assumes_balancer_off,
 * ]
 */

let testDb = db.getSiblingDB("convert_to_capped");
let coll = testDb.coll;
testDb.dropDatabase();

// Create a collection with some data.
let num = 10;
for (let i = 0; i < num; ++i) {
    assert.commandWorked(coll.insert({_id: i}));
}

// Ensure we do not allow overflowing the size long long on the server (SERVER-33078).
assert.commandFailedWithCode(
    testDb.runCommand({convertToCapped: coll.getName(), size: 5308156746568725891247}),
    ErrorCodes.BadValue);
