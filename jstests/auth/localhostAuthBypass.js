// SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
// This test is to ensure that localhost authentication works correctly against a standalone
// bongod whether it is hosted with "localhost" or a hostname.

var baseName = "auth_server-6591";
var dbpath = BongoRunner.dataPath + baseName;
var username = "foo";
var password = "bar";

load("jstests/libs/host_ipaddr.js");

var createUser = function(bongo) {
    print("============ adding a user.");
    bongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

var createRole = function(bongo) {
    print("============ adding a role.");
    bongo.getDB("admin").createRole(
        {role: "roleAdministrator", roles: [{role: "userAdmin", db: "admin"}], privileges: []});
};

var assertCannotRunCommands = function(bongo) {
    print("============ ensuring that commands cannot be run.");

    var test = bongo.getDB("test");
    assert.throws(function() {
        test.system.users.findOne();
    });

    assert.writeError(test.foo.save({_id: 0}));

    assert.throws(function() {
        test.foo.findOne({_id: 0});
    });

    assert.writeError(test.foo.update({_id: 0}, {$set: {x: 20}}));
    assert.writeError(test.foo.remove({_id: 0}));

    assert.throws(function() {
        test.foo.mapReduce(
            function() {
                emit(1, 1);
            },
            function(id, count) {
                return Array.sum(count);
            },
            {out: "other"});
    });

    // Additional commands not permitted
    // Create non-admin user
    assert.throws(function() {
        bongo.getDB("test").createUser({user: username, pwd: password, roles: ['readWrite']});
    });
    // DB operations
    var authorizeErrorCode = 13;
    assert.commandFailedWithCode(
        bongo.getDB("test").copyDatabase("admin", "admin2"), authorizeErrorCode, "copyDatabase");
    // Create collection
    assert.commandFailedWithCode(
        bongo.getDB("test").createCollection("log", {capped: true, size: 5242880, max: 5000}),
        authorizeErrorCode,
        "createCollection");
    // Set/Get system parameters
    var params = [
        {param: "journalCommitInterval", val: 200},
        {param: "logLevel", val: 2},
        {param: "logUserIds", val: 1},
        {param: "notablescan", val: 1},
        {param: "quiet", val: 1},
        {param: "replApplyBatchSize", val: 10},
        {param: "replIndexPrefetch", val: "none"},
        {param: "syncdelay", val: 30},
        {param: "traceExceptions", val: true},
        {param: "sslMode", val: "preferSSL"},
        {param: "clusterAuthMode", val: "sendX509"},
        {param: "userCacheInvalidationIntervalSecs", val: 300}
    ];
    params.forEach(function(p) {
        var cmd = {setParameter: 1};
        cmd[p.param] = p.val;
        assert.commandFailedWithCode(
            bongo.getDB("admin").runCommand(cmd), authorizeErrorCode, "setParameter: " + p.param);
    });
    params.forEach(function(p) {
        var cmd = {getParameter: 1};
        cmd[p.param] = 1;
        assert.commandFailedWithCode(
            bongo.getDB("admin").runCommand(cmd), authorizeErrorCode, "getParameter: " + p.param);
    });
};

var assertCanRunCommands = function(bongo) {
    print("============ ensuring that commands can be run.");

    var test = bongo.getDB("test");
    // will throw on failure
    test.system.users.findOne();

    assert.writeOK(test.foo.save({_id: 0}));
    assert.writeOK(test.foo.update({_id: 0}, {$set: {x: 20}}));
    assert.writeOK(test.foo.remove({_id: 0}));

    test.foo.mapReduce(
        function() {
            emit(1, 1);
        },
        function(id, count) {
            return Array.sum(count);
        },
        {out: "other"});
};

var authenticate = function(bongo) {
    print("============ authenticating user.");
    bongo.getDB("admin").auth(username, password);
};

var shutdown = function(conn) {
    print("============ shutting down.");
    BongoRunner.stopBongod(conn.port, /*signal*/ false, {auth: {user: username, pwd: password}});
};

var runTest = function(useHostName) {
    print("==========================");
    print("starting bongod: useHostName=" + useHostName);
    print("==========================");
    var conn = BongoRunner.runBongod({auth: "", dbpath: dbpath, useHostName: useHostName});

    var bongo = new Bongo("localhost:" + conn.port);

    assertCannotRunCommands(bongo);

    createUser(bongo);

    assertCannotRunCommands(bongo);

    authenticate(bongo);

    assertCanRunCommands(bongo);

    print("============ reconnecting with new client.");
    bongo = new Bongo("localhost:" + conn.port);

    assertCannotRunCommands(bongo);

    authenticate(bongo);

    assertCanRunCommands(bongo);

    shutdown(conn);
};

var runNonlocalTest = function(host) {
    print("==========================");
    print("starting bongod: non-local host access " + host);
    print("==========================");
    var conn = BongoRunner.runBongod({auth: "", dbpath: dbpath});

    var bongo = new Bongo(host + ":" + conn.port);

    assertCannotRunCommands(bongo);
    assert.throws(function() {
        bongo.getDB("admin").createUser(
            {user: username, pwd: password, roles: jsTest.adminUserRoles});
    });
    assert.throws(function() {
        bongo.getDB("$external")
            .createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
    });
    shutdown(conn);
};

// Per SERVER-23503, the existence of roles in the admin database should disable the localhost
// exception.
// Start the server without auth. Create a role. Restart the server with auth. The exception is
// now enabled.
var runRoleTest = function() {
    var conn = BongoRunner.runBongod({dbpath: dbpath});
    var bongo = new Bongo("localhost:" + conn.port);
    assertCanRunCommands(bongo);
    createRole(bongo);
    assertCanRunCommands(bongo);
    BongoRunner.stopBongod(conn);
    conn = BongoRunner.runBongod({auth: '', dbpath: dbpath, restart: true, cleanData: false});
    bongo = new Bongo("localhost:" + conn.port);
    assertCannotRunCommands(bongo);
};

runTest(false);
runTest(true);

runNonlocalTest(get_ipaddr());

runRoleTest();
