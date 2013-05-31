var conn1 = MongoRunner.runMongod({ auth: "",
                                    smallfiles: "",
                                    setParameter: "enableLocalhostAuthBypass=true"});
var conn2 = MongoRunner.runMongod({ auth: "",
                                    smallfiles: "",
                                    setParameter: "enableLocalhostAuthBypass=false"});

// Should succeed because of localhost exception
conn1.getDB("test").foo.insert({a:1});
assert.eq(1, conn1.getDB("test").foo.findOne().a);

// Should fail since localhost exception is disabled
assert.throws(function() { conn2.getDB("test").foo.findOne(); });