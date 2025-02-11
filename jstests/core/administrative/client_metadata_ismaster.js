// Test that verifies client metadata behavior for isMaster

// Verify that a isMaster request fails if it contains client metadata, and it is not first.
// The shell sends isMaster on the first connection
var result = db.runCommand({"isMaster": 1, "client": {"application": "foobar"}});
assert.commandFailed(result);
assert.eq(result.code, ErrorCodes.ClientMetadataCannotBeMutated, tojson(result));