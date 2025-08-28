// SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
// @tags: [requires_os_access]
// This test is to ensure that localhost authentication works correctly against a replica set
// whether they are hosted with "localhost" or a hostname.

let replSetName = "replsets_server-6591";
let keyfile = "jstests/libs/key1";
let username = "foo";
let password = "bar";

import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

let createUser = function (mongo) {
    print("============ adding a user.");
    mongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

let assertCannotRunCommands = function (mongo, isPrimary) {
    print("============ ensuring that commands cannot be run.");

    let test = mongo.getDB("test");
    assert.throws(function () {
        test.system.users.findOne();
    });
    assert.throws(function () {
        test.foo.findOne({_id: 0});
    });

    if (isPrimary) {
        assert.writeError(test.foo.save({_id: 0}));
        assert.writeError(test.foo.update({_id: 0}, {$set: {x: 20}}));
        assert.writeError(test.foo.remove({_id: 0}));
    }

    assert.throws(function () {
        test.foo.mapReduce(
            function () {
                emit(1, 1);
            },
            function (id, count) {
                return Array.sum(count);
            },
            {out: "other"},
        );
    });

    // Create collection
    let authorizeErrorCode = 13;
    assert.commandFailedWithCode(
        mongo.getDB("test").createCollection("log", {capped: true, size: 5242880, max: 5000}),
        authorizeErrorCode,
        "createCollection",
    );
    // Set/Get system parameters
    let params = [
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
        {param: "userCacheInvalidationIntervalSecs", val: 300},
    ];
    params.forEach(function (p) {
        let cmd = {setParameter: 1};
        cmd[p.param] = p.val;
        assert.commandFailedWithCode(
            mongo.getDB("admin").runCommand(cmd),
            authorizeErrorCode,
            "setParameter: " + p.param,
        );
    });
    params.forEach(function (p) {
        let cmd = {getParameter: 1};
        cmd[p.param] = 1;
        assert.commandFailedWithCode(
            mongo.getDB("admin").runCommand(cmd),
            authorizeErrorCode,
            "getParameter: " + p.param,
        );
    });
};

let assertCanRunCommands = function (mongo) {
    print("============ ensuring that commands can be run.");

    let test = mongo.getDB("test");
    // will throw on failure
    test.system.users.findOne();

    assert.commandWorked(test.foo.save({_id: 0}));
    assert.commandWorked(test.foo.update({_id: 0}, {$set: {x: 20}}));
    assert.commandWorked(test.foo.remove({_id: 0}));

    test.foo.mapReduce(
        function () {
            emit(1, 1);
        },
        function (id, count) {
            return Array.sum(count);
        },
        {out: "other"},
    );

    assert.commandWorked(mongo.getDB("admin").runCommand({replSetGetStatus: 1}));
};

let authenticate = function (mongo) {
    print("============ authenticating user.");
    mongo.getDB("admin").auth(username, password);
};

let start = function (useHostName) {
    const rs = new ReplSetTest({name: replSetName, nodes: 3, keyFile: keyfile, auth: "", useHostName: useHostName});

    rs.startSet();
    rs.initiate();
    return rs;
};

let shutdown = function (rs) {
    print("============ shutting down.");
    rs.stopSet(/*signal*/ false, /*forRestart*/ false, {auth: {user: username, pwd: password}});
};

let runTest = function (useHostName) {
    print("=====================");
    print("starting replica set: useHostName=" + useHostName);
    print("=====================");
    const rs = start(useHostName);
    let port = rs.getPort(rs.getPrimary());
    let host = "localhost:" + port;
    let secHosts = [];

    rs.getSecondaries().forEach(function (sec) {
        secHosts.push("localhost:" + rs.getPort(sec));
    });

    let mongo = new Mongo(host);

    assertCannotRunCommands(mongo, true);

    // Test localhost access on secondaries
    let mongoSecs = [];
    secHosts.forEach(function (h) {
        mongoSecs.push(new Mongo(h));
    });

    mongoSecs.forEach(function (m) {
        assertCannotRunCommands(m, false);
    });

    createUser(mongo);

    assertCannotRunCommands(mongo, true);

    authenticate(mongo);

    assertCanRunCommands(mongo, true);

    // Test localhost access on secondaries on exsiting connection
    mongoSecs.forEach(function (m) {
        assertCannotRunCommands(m, false);
        authenticate(m);
    });

    print("===============================");
    print("reconnecting with a new client.");
    print("===============================");

    mongo = new Mongo(host);

    assertCannotRunCommands(mongo, true);

    authenticate(mongo);

    assertCanRunCommands(mongo, true);

    // Test localhost access on secondaries on new connection
    secHosts.forEach(function (h) {
        let m = new Mongo(h);
        assertCannotRunCommands(m, false);
        authenticate(m);
    });

    shutdown(rs);
};

let runNonlocalTest = function (ipAddr) {
    print("==========================");
    print("starting mongod: non-local host access " + ipAddr);
    print("==========================");

    const rs = start(false);
    let port = rs.getPort(rs.getPrimary());
    let host = ipAddr + ":" + port;
    let secHosts = [];

    rs.getSecondaries().forEach(function (sec) {
        secHosts.push(ipAddr + ":" + rs.getPort(sec));
    });

    let mongo = new Mongo(host);

    assertCannotRunCommands(mongo, true);

    // Test localhost access on secondaries
    let mongoSecs = [];
    secHosts.forEach(function (h) {
        mongoSecs.push(new Mongo(h));
    });

    mongoSecs.forEach(function (m) {
        assertCannotRunCommands(m, false);
    });

    assert.throws(function () {
        mongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
    });

    shutdown(rs);
};

runTest(false);
runTest(true);

runNonlocalTest(get_ipaddr());
