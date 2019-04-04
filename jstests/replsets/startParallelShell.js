// Test startParallelShell() in a replica set.

var db;

(function() {
    'use strict';

    const setName = 'rs0';
    const replSet = new ReplSetTest({name: setName, nodes: 3});
    const nodes = replSet.nodeList();
    replSet.startSet();
    replSet.initiate();

    const url = replSet.getURL();
    print("* Connecting to " + url);
    const merizo = new Merizo(url);
    db = merizo.getDB('admin');
    assert.eq(url, merizo.host, "replSet.getURL() should match active connection string");

    print("* Starting parallel shell on --host " + db.getMerizo().host);
    var awaitShell = startParallelShell('db.coll0.insert({test: "connString only"});');
    assert.soon(function() {
        return db.coll0.find({test: "connString only"}).count() === 1;
    });
    awaitShell();

    const uri = new MerizoURI(url);
    const port0 = uri.servers[0].port;
    print("* Starting parallel shell w/ --port " + port0);
    awaitShell = startParallelShell('db.coll0.insert({test: "explicit port"});', port0);
    assert.soon(function() {
        return db.coll0.find({test: "explicit port"}).count() === 1;
    });
    awaitShell();
    replSet.stopSet();
})();
