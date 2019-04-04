// @tags: [requires_non_retryable_commands]

// Test that cleanupOrphaned cannot be run on stand alone merizod.
var res = db.adminCommand({cleanupOrphaned: 'unsharded.coll'});
assert(!res.ok, tojson(res));
