/**
 * Test that the "wrapped" command format using {$query: <cmd>} fails cleanly.
 *
 * The test runs commands that are not allowed with security token: query.
 * @tags: [
 *    requires_fcv_63,
 *    not_allowed_with_security_token,
 * ]
 */
(function() {
"use strict";
// The OP_QUERY protocol used to permit commands to be wrapped inside a key named "$query" or
// "query". Even if the client was using OP_MSG, the client code used to have logic to upconvert
// this OP_QUERY format to a correct OP_MSG.
//
// The server no longer supports OP_QUERY, and the upconversion logic has been deleted. Therefore,
// the "wrapped" format should be forwarded to the server unadulterated, and the server should
// produce an "unknown command" error.
assert.commandFailedWithCode(db.runCommand({$query: {ping: 1}}), ErrorCodes.CommandNotFound);
assert.commandFailedWithCode(db.runCommand({query: {ping: 1}}), ErrorCodes.CommandNotFound);
}());
