//SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
//This test is to ensure that localhost authentication works correctly against a replica set 
//whether they are hosted with "localhost" or a hostname.

var replSetName = "replsets_server-6591";
var keyfile = "jstests/libs/key1";
var memberCount = 3;
var username = "foo";
var password = "bar";

var createUser = function(mongo) {
    print("============ adding a user.");
    mongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

var assertCannotRunCommands = function(mongo) {
    print("============ ensuring that commands cannot be run.");

    var test = mongo.getDB("test");
    assert.throws( function() { test.system.users.findOne(); });
    assert.throws( function() { test.foo.findOne({ _id: 0 }); });
        
    assert.writeError(test.foo.save({ _id: 0 }));
    assert.writeError(test.foo.update({ _id: 0 }, { $set: { x: 20 }}));
    assert.writeError(test.foo.remove({ _id: 0 }));

    assert.throws(function() { 
        test.foo.mapReduce(
            function() { emit(1, 1); }, 
            function(id, count) { return Array.sum(count); },
            { out: "other" });
    });
};

var assertCanRunCommands = function(mongo) {
    print("============ ensuring that commands can be run.");

    var test = mongo.getDB("test");
    // will throw on failure
    test.system.users.findOne();

    assert.writeOK(test.foo.save({_id: 0 }));
    assert.writeOK(test.foo.update({ _id: 0 }, { $set: { x: 20 }}));
    assert.writeOK(test.foo.remove({ _id: 0 }));

    test.foo.mapReduce(
        function() { emit(1, 1); }, 
        function(id, count) { return Array.sum(count); },
        { out: "other" }
    );
};

var authenticate = function(mongo) {
    print("============ authenticating user.");
    mongo.getDB("admin").auth(username, password);
};

var start = function(useHostName) {
    var rs = new ReplSetTest({name: replSetName, 
        nodes : 3, 
        keyFile : keyfile,  
        useHostName: useHostName});

    rs.startSet();
    rs.initiate();
    return rs;
};

var shutdown = function(rs) {
    print("============ shutting down.");
    rs.stopSet(/*signal*/false, 
        /*forRestart*/false, 
        { auth: { user: username, pwd: password}});
};

var runTest = function(useHostName) {
    print("=====================");
    print("starting replica set: useHostName=" + useHostName);
    print("=====================");
    var rs = start(useHostName);
    var port = rs.getPort(rs.getPrimary());
    var host = "localhost:" + port;

    var mongo = new Mongo(host);

    assertCanRunCommands(mongo);

    createUser(mongo);

    assertCannotRunCommands(mongo);

    authenticate(mongo);

    assertCanRunCommands(mongo);

    print("===============================");
    print("reconnecting with a new client.");
    print("===============================");

    mongo = new Mongo(host);

    assertCannotRunCommands(mongo);

    authenticate(mongo);

    assertCanRunCommands(mongo);

    shutdown(rs);
}

runTest(false);
runTest(true);
