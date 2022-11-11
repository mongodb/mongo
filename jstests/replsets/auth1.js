// check replica set authentication
//
// This test requires users to persist across a restart.
// @tags: [requires_persistence, multiversion_incompatible]

load("jstests/replsets/rslib.js");

(function() {
"use strict";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

var name = "rs_auth1";
var port = allocatePorts(5);
var path = "jstests/libs/";

// These keyFiles have their permissions set to 600 later in the test.
var key1_600 = path + "key1";
var key2_600 = path + "key2";

// This keyFile has its permissions set to 644 later in the test.
var key1_644 = path + "key1_644";

print("try starting mongod with auth");
var m =
    MongoRunner.runMongod({auth: "", port: port[4], dbpath: MongoRunner.dataDir + "/wrong-auth"});

assert(!m.getDB("local").auth("__system", ""));

MongoRunner.stopMongod(m);

if (!_isWindows()) {
    print("reset permissions");
    run("chmod", "644", key1_644);

    print("try starting mongod");
    m = runMongoProgram("mongod",
                        "--keyFile",
                        key1_644,
                        "--port",
                        port[0],
                        "--dbpath",
                        MongoRunner.dataPath + name);

    print("should fail with wrong permissions");
    assert.eq(m, 1, "mongod should exit w/ 1 (EXIT_FAILURE): permissions too open");
}

// Pre-populate the data directory for the first replica set node, to be started later, with
// a user's credentials.
print("add a user to server0: foo");
m = MongoRunner.runMongod({dbpath: MongoRunner.dataPath + name + "-0"});
m.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
m.getDB("test").createUser({user: "bar", pwd: "baz", roles: jsTest.basicUserRoles});
print("make sure user is written before shutting down");
MongoRunner.stopMongod(m);

print("start up rs");
var rs = new ReplSetTest({"name": name, "nodes": 3});

// The first node is started with the pre-populated data directory.
print("start 0 with keyFile");
m = rs.start(0, {"keyFile": key1_600, noCleanData: true});
print("start 1 with keyFile");
rs.start(1, {"keyFile": key1_600});
print("start 2 with keyFile");
rs.start(2, {"keyFile": key1_600});

assert(m.getDB("admin").auth("foo", "bar"));
print("Initializing replSet with config: " + tojson(rs.getReplSetConfig()));
assert.commandWorked(m.getDB("admin").runCommand({replSetInitiate: rs.getReplSetConfig()}));
rs.awaitNodesAgreeOnPrimaryNoAuth();

m.getDB('admin').logout();  // In case this node doesn't become primary, make sure its not auth'd

var primary = rs.getPrimary();
rs.awaitSecondaryNodes();
var mId = rs.getNodeId(primary);
var secondary = rs.getSecondary();
assert(primary.getDB("admin").auth("foo", "bar"));
assert.commandWorked(primary.getDB("test").foo.insert(
    {x: 1}, {writeConcern: {w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}}));

print("try some legal and illegal reads");
var r = primary.getDB("test").foo.findOne();
assert.eq(r.x, 1);

secondary.setSecondaryOk();

function doQueryOn(p) {
    var error = assert.throws(function() {
                                 r = p.getDB("test").foo.findOne();
                             }, [], "find did not throw, returned: " + tojson(r)).toString();
    printjson(error);
    assert.gt(error.indexOf("Command find requires authentication"), -1, "error was non-auth");
}

doQueryOn(secondary);
primary.getDB("admin").logout();

print("unauthorized:");
printjson(primary.adminCommand({replSetGetStatus: 1}));

assert(secondary.getDB("test").auth("bar", "baz"));
r = secondary.getDB("test").foo.findOne();
assert.eq(r.x, 1);
secondary.getDB('test').logout();

print("add some data");
assert(primary.getDB("test").auth("bar", "baz"));
var bulk = primary.getDB("test").foo.initializeUnorderedBulkOp();
for (var i = 0; i < 1000; i++) {
    bulk.insert({x: i, foo: "bar"});
}
assert.commandWorked(bulk.execute({w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS}));
primary.getDB("test").logout();

print("fail over");
rs.stop(mId);

primary = rs.getPrimary();

print("add some more data 1");
assert(primary.getDB("test").auth("bar", "baz"));
bulk = primary.getDB("test").foo.initializeUnorderedBulkOp();
for (var i = 0; i < 1000; i++) {
    bulk.insert({x: i, foo: "bar"});
}
assert.commandWorked(bulk.execute({w: 2}));
primary.getDB("test").logout();

print("resync");
rs.restart(mId, {"keyFile": key1_600});
rs.keyFile = key1_600;
primary = rs.getPrimary();

print("add some more data 2");
assert(primary.getDB("test").auth("bar", "baz"));
bulk = primary.getDB("test").foo.initializeUnorderedBulkOp();
for (var i = 0; i < 1000; i++) {
    bulk.insert({x: i, foo: "bar"});
}
bulk.execute({w: 3, wtimeout: ReplSetTest.kDefaultTimeoutMS});
primary.getDB("test").logout();

print("add member with wrong key");
var conn = MongoRunner.runMongod({
    dbpath: MongoRunner.dataPath + name + "-3",
    port: port[3],
    replSet: "rs_auth1",
    oplogSize: 2,
    keyFile: key2_600
});

assert(primary.getDB("admin").auth("foo", "bar"));
var config = primary.getDB("local").system.replset.findOne();
config.members.push({_id: 3, host: rs.host + ":" + port[3]});
config.version++;
try {
    primary.adminCommand({replSetReconfig: config});
} catch (e) {
    print("error: " + e);
}
primary.getDB("admin").logout();

primary = rs.getPrimary();
assert(primary.getDB("admin").auth("foo", "bar"));

print("shouldn't ever sync");
for (var i = 0; i < 10; i++) {
    print("iteration: " + i);
    var results = primary.adminCommand({replSetGetStatus: 1});
    printjson(results);
    assert(results.members[3].state != 2);
    sleep(1000);
}
primary.getDB("admin").logout();

print("stop member");
MongoRunner.stopMongod(conn);

print("start back up with correct key");
var conn = MongoRunner.runMongod({
    dbpath: MongoRunner.dataPath + name + "-3",
    port: port[3],
    replSet: "rs_auth1",
    oplogSize: 2,
    keyFile: key1_600
});

assert(primary.getDB('admin').auth("foo", "bar"));
wait(function() {
    try {
        var results = primary.adminCommand({replSetGetStatus: 1});
        printjson(results);
        return results.members[3].state == 2;
    } catch (e) {
        print(e);
    }
    return false;
});
primary.getDB('admin').logout();

print("make sure it has the config, too");
assert.soon(function() {
    for (var i in rs.nodes) {
        // Make sure there are no lingering logins on the test database.
        rs.nodes[i].getDB('test').logout();

        rs.nodes[i].setSecondaryOk();
        assert(rs.nodes[i].getDB("admin").auth("foo", "bar"));
        config = rs.nodes[i].getDB("local").system.replset.findOne();
        rs.nodes[i].getDB("admin").logout();
        // We expect the config version to be 3 due to the initial config and then the
        // 'newlyAdded' removal reconfig.
        if (config.version !== 3) {
            return false;
        }
    }
    return true;
});
MongoRunner.stopMongod(conn);
rs.stopSet();
})();
