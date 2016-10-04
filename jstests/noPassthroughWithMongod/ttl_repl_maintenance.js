/** This tests ensures that when a stand-alone server is started with something in
 *  local.system.replset, it doesn't start the TTL monitor (SERVER-6609). The test creates a
 *  dummy replset config & TTL collection, then restarts the member and ensures that it doesn't
 *  time out the docs in the TTL collection. Then it removes the "config" and
 *  restarts, ensuring that the TTL monitor deletes the docs.
 */

var runner;
var conn;

var primeSystemReplset = function() {
    conn = MongoRunner.runMongod();
    var localDB = conn.getDB("local");
    localDB.system.replset.insert({x: 1});

    print("create a TTL collection");
    var testDB = conn.getDB("test");
    assert.commandWorked(testDB.foo.ensureIndex({x: 1}, {expireAfterSeconds: 2}));
};

var restartWithConfig = function() {
    MongoRunner.stopMongod(conn.port, 15);
    conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});
    testDB = conn.getDB("test");
    var n = 100;
    for (var i = 0; i < n; i++) {
        testDB.foo.insert({x: new Date()});
    }

    print("sleeping 65 seconds");
    sleep(65000);

    assert.eq(testDB.foo.count(), n);
};

var restartWithoutConfig = function() {
    var localDB = conn.getDB("local");
    assert.writeOK(localDB.system.replset.remove({}));

    MongoRunner.stopMongod(conn.port, 15);

    conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});

    assert.soon(function() {
        return conn.getDB("test").foo.count() < 100;
    }, "never deleted", 75000);

    MongoRunner.stopMongod(conn.port, 15);
};

print("Create a TTL collection and put doc in local.system.replset");
primeSystemReplset();

print("make sure TTL doesn't work when member is started with system.replset doc");
restartWithConfig();

print("remove system.replset entry & restart");
restartWithoutConfig();
