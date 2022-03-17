// This test expects to be run against a non-shardsvr.
// @tags: [
//  requires_non_retryable_commands,
//  directly_against_shardsvrs_incompatible,
// ]

// Test that cleanupOrphaned cannot be run on stand alone mongod.
var res = db.adminCommand({cleanupOrphaned: 'unsharded.coll'});
assert(!res.ok, tojson(res));
