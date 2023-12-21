// @tags: [
//  # The test runs commands that are not allowed with security token: cleanupOrphaned.
//  not_allowed_with_signed_security_token,
//  requires_non_retryable_commands,
//  # This test expects to be run against a non-shardsvr.
//  directly_against_shardsvrs_incompatible,
//  assumes_standalone_mongod,
// ]

// Test that cleanupOrphaned cannot be run on stand alone mongod.
var res = db.adminCommand({cleanupOrphaned: 'unsharded.coll'});
assert(!res.ok, tojson(res));
