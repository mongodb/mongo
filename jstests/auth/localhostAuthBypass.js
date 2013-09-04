//SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
//This test is to ensure that localhost authentication works correctly against a standalone 
//mongod whether it is hosted with "localhost" or a hostname.

var baseName = "auth_server-6591";
var dbpath = "/data/db/" + baseName;
var username = "foo";
var password = "bar";
var port = allocatePorts(1)[0];
var host = "localhost:" + port;

var addUser = function(mongo) {
    print("============ adding a user.");
    mongo.getDB("admin").addUser(username, password, jsTest.adminUserRoles);
};

var assertCannotRunCommands = function(mongo) {
    print("============ ensuring that commands cannot be run.");

    var test = mongo.getDB("test");
    assert.throws( function() { test.system.users.findOne(); });

    test.foo.save({_id:0});
    assert(test.getLastError());
    
    assert.throws( function() { test.foo.findOne({_id:0}); });
    
    test.foo.update({_id:0}, {$set:{x:20}});
    assert(test.getLastError());
    
    test.foo.remove({_id:0});
    assert(test.getLastError());

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

    test.foo.save({_id: 0});
    assert(test.getLastError() == null);
    
    test.foo.update({_id: 0}, {$set:{x:20}});
    assert(test.getLastError() == null);
    
    test.foo.remove({_id: 0});
    assert(test.getLastError() == null);
    
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

var shutdown = function(mongo) {
    print("============ shutting down.");
    MongoRunner.stopMongod(port, /*signal*/false, { auth: { user: username, pwd: password}});
};

var runTest = function(useHostName) {
    print("==========================");
    print("starting mongod: useHostName=" + useHostName);
    print("==========================");
    MongoRunner.runMongod({auth: "", port: port, dbpath: dbpath, useHostName: useHostName});

    var mongo = new Mongo(host);

    assertCanRunCommands(mongo);

    addUser(mongo);

    assertCannotRunCommands(mongo);

    authenticate(mongo);

    assertCanRunCommands(mongo);

    print("============ reconnecting with new client.");
    mongo = new Mongo(host);

    assertCannotRunCommands(mongo);

    authenticate(mongo);

    assertCanRunCommands(mongo);

    shutdown(mongo);
};

runTest(false);
runTest(true);