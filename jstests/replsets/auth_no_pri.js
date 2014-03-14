// Test that you can still authenticate a replset connection to a RS with no primary (SERVER-6665).
var rs = new ReplSetTest({"nodes" : 3, keyFile : "jstests/libs/key1"});
rs.startSet();
rs.initiate();

// Add user
var master = rs.getMaster();
master.getDB("admin").createUser({user: "admin", pwd: "pwd", roles: ["root"]}, {w: 'majority'});

// Can authenticate replset connection when whole set is up.
var conn = new Mongo(rs.getURL());
assert(conn.getDB('admin').auth('admin', 'pwd'));
conn.getDB('admin').foo.insert({a:1});
assert.gleSuccess(conn.getDB('admin'));

// Make sure there is no primary
rs.stop(0);
rs.stop(1);
assert.throws(function() {rs.getMaster()}); // Should no longer be any primary

// Make sure you can still authenticate a replset connection with no primary
var conn2 = new Mongo(rs.getURL());
conn2.setSlaveOk(true);
assert(conn2.getDB('admin').auth('admin', 'pwd'));
assert.eq(1, conn2.getDB('admin').foo.findOne().a);

rs.stopSet();