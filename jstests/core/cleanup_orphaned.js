// Test that cleanupOrphaned cannot be run on stand alone mongod.
var res = db.adminCommand({cleanupOrphaned: 'unsharded.coll'});
assert(!res.ok, tojson(res));
