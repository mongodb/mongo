/**
 * This tests ensures that when a stand-alone server is started with something in
 *  local.system.replset, it doesn't start the TTL monitor (SERVER-6609). The test creates a
 *  dummy replset config & TTL collection, then restarts the member and ensures that it doesn't
 *  time out the docs in the TTL collection. Then it removes the "config" and
 *  restarts, ensuring that the TTL monitor deletes the docs.
 */

let runner;
let conn;

let primeSystemReplset = function () {
    conn = MongoRunner.runMongod();
    let localDB = conn.getDB("local");
    localDB.system.replset.insert({x: 1});

    print("create a TTL collection");
    let testDB = conn.getDB("test");
    assert.commandWorked(testDB.foo.createIndex({x: 1}, {expireAfterSeconds: 2}));
};

let restartWithConfig = function () {
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});
    let testDB = conn.getDB("test");
    let n = 100;
    for (let i = 0; i < n; i++) {
        testDB.foo.insert({x: new Date()});
    }

    print("sleeping 65 seconds");
    sleep(65000);

    assert.eq(testDB.foo.count(), n);
};

let restartWithoutConfig = function () {
    let localDB = conn.getDB("local");
    assert.commandWorked(localDB.system.replset.remove({}));

    MongoRunner.stopMongod(conn);

    conn = MongoRunner.runMongod({restart: true, cleanData: false, dbpath: conn.dbpath});

    assert.soon(
        function () {
            return conn.getDB("test").foo.count() < 100;
        },
        "never deleted",
        75000,
    );

    MongoRunner.stopMongod(conn);
};

print("Create a TTL collection and put doc in local.system.replset");
primeSystemReplset();

print("make sure TTL doesn't work when member is started with system.replset doc");
restartWithConfig();

print("remove system.replset entry & restart");
restartWithoutConfig();
