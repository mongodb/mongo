/**
 * Checks that listCommands returns the API Version information of a command.
 *
 * @tags: [
 *   requires_non_retryable_commands,
 *   no_selinux,
 * ]
 */

(function() {
"use strict";

const resListCommands = db.runCommand({listCommands: 1});
assert.commandWorked(resListCommands);
const {find, serverStatus, testDeprecation} = resListCommands["commands"];
assert(JSON.stringify(find.apiVersions) == "[\"1\"]");
assert(JSON.stringify(find.deprecatedApiVersions) == "[]");
assert(JSON.stringify(serverStatus.apiVersions) == "[]");
assert(JSON.stringify(serverStatus.deprecatedApiVersions) == "[]");
assert(JSON.stringify(testDeprecation.apiVersions) == "[\"1\"]");
assert(JSON.stringify(testDeprecation.deprecatedApiVersions) == "[\"1\"]");
})();
