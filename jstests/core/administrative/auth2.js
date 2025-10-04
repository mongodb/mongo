// just make sure logout doesn't break anything
//
// @tags: [
//   # The test runs commands that are not allowed with security token: logout.
//   not_allowed_with_signed_security_token,
//   requires_auth,
//   requires_non_retryable_commands
// ]

// SERVER-724
db.runCommand({logout: 1});
let x = db.runCommand({logout: 1});
assert.eq(1, x.ok, "A");

x = db.logout();
assert.eq(1, x.ok, "B");
