// Test that cleanupOrphaned cannot be run on stand alone bongod.
var res = db.adminCommand({cleanupOrphaned: 'unsharded.coll'});
assert(!res.ok, tojson(res));
