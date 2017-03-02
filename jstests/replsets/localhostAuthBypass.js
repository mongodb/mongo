// SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
// This test is to ensure that localhost authentication works correctly against a replica set
// whether they are hosted with "localhost" or a hostname.

var replSetName = "replsets_server-6591";
var keyfile = "jstests/libs/key1";
var username = "foo";
var password = "bar";

load("jstests/libs/host_ipaddr.js");

var createUser = function(bongo) {
    print("============ adding a user.");
    bongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

var assertCannotRunCommands = function(bongo, isPrimary) {
    print("============ ensuring that commands cannot be run.");

    var test = bongo.getDB("test");
    assert.throws(function() {
        test.system.users.findOne();
    });
    assert.throws(function() {
        test.foo.findOne({_id: 0});
    });

    if (isPrimary) {
        assert.writeError(test.foo.save({_id: 0}));
        assert.writeError(test.foo.update({_id: 0}, {$set: {x: 20}}));
        assert.writeError(test.foo.remove({_id: 0}));
    }

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

    assert.commandWorked(bongo.getDB("admin").runCommand({replSetGetStatus: 1}));
};

var authenticate = function(bongo) {
    print("============ authenticating user.");
    bongo.getDB("admin").auth(username, password);
};

var start = function(useHostName) {
    var rs = new ReplSetTest(
        {name: replSetName, nodes: 3, keyFile: keyfile, auth: "", useHostName: useHostName});

    rs.startSet();
    rs.initiate();
    return rs;
};

var shutdown = function(rs) {
    print("============ shutting down.");
    rs.stopSet(/*signal*/ false,
               /*forRestart*/ false,
               {auth: {user: username, pwd: password}});
};

var runTest = function(useHostName) {
    print("=====================");
    print("starting replica set: useHostName=" + useHostName);
    print("=====================");
    var rs = start(useHostName);
    var port = rs.getPort(rs.getPrimary());
    var host = "localhost:" + port;
    var secHosts = [];

    rs.getSecondaries().forEach(function(sec) {
        secHosts.push("localhost:" + rs.getPort(sec));
    });

    var bongo = new Bongo(host);

    assertCannotRunCommands(bongo, true);

    // Test localhost access on secondaries
    var bongoSecs = [];
    secHosts.forEach(function(h) {
        bongoSecs.push(new Bongo(h));
    });

    bongoSecs.forEach(function(m) {
        assertCannotRunCommands(m, false);
    });

    createUser(bongo);

    assertCannotRunCommands(bongo, true);

    authenticate(bongo);

    assertCanRunCommands(bongo, true);

    // Test localhost access on secondaries on exsiting connection
    bongoSecs.forEach(function(m) {
        assertCannotRunCommands(m, false);
        authenticate(m);
    });

    print("===============================");
    print("reconnecting with a new client.");
    print("===============================");

    bongo = new Bongo(host);

    assertCannotRunCommands(bongo, true);

    authenticate(bongo);

    assertCanRunCommands(bongo, true);

    // Test localhost access on secondaries on new connection
    secHosts.forEach(function(h) {
        var m = new Bongo(h);
        assertCannotRunCommands(m, false);
        authenticate(m);
    });

    shutdown(rs);
};

var runNonlocalTest = function(ipAddr) {
    print("==========================");
    print("starting bongod: non-local host access " + ipAddr);
    print("==========================");

    var rs = start(false);
    var port = rs.getPort(rs.getPrimary());
    var host = ipAddr + ":" + port;
    var secHosts = [];

    rs.getSecondaries().forEach(function(sec) {
        secHosts.push(ipAddr + ":" + rs.getPort(sec));
    });

    var bongo = new Bongo(host);

    assertCannotRunCommands(bongo, true);

    // Test localhost access on secondaries
    var bongoSecs = [];
    secHosts.forEach(function(h) {
        bongoSecs.push(new Bongo(h));
    });

    bongoSecs.forEach(function(m) {
        assertCannotRunCommands(m, false);
    });

    assert.throws(function() {
        bongo.getDB("admin").createUser(
            {user: username, pwd: password, roles: jsTest.adminUserRoles});
    });

    shutdown(rs);
};

runTest(false);
runTest(true);

runNonlocalTest(get_ipaddr());
