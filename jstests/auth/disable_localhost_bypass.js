var conn = MongoRunner.runMongod({ auth: "", smallfiles: ""});

var test = conn.getDB("test");

test.foo.insert({a:1});
assert.eq(1, test.foo.findOne().a);

conn.getDB("admin").runCommand({setParameter:1, enableLocalhostAuthBypass:false});

assert.throws(function() { db.foo.findOne(); });