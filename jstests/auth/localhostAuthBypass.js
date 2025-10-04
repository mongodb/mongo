// SERVER-6591: Localhost authentication exception doesn't work right on sharded cluster
//
// This test is to ensure that localhost authentication works correctly against a standalone
// mongod whether it is hosted with "localhost" or a hostname.
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";

let baseName = "auth_server-6591";
let dbpath = MongoRunner.dataPath + baseName;
let username = "foo";
let password = "bar";

let createUser = function (db) {
    print("============ adding a user.");
    db.createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
};

let createRole = function (mongo) {
    print("============ adding a role.");
    mongo
        .getDB("admin")
        .createRole({role: "roleAdministrator", roles: [{role: "userAdmin", db: "admin"}], privileges: []});
};

let assertCannotRunCommands = function (mongo) {
    print("============ ensuring that commands cannot be run.");

    let test = mongo.getDB("test");
    assert.throws(function () {
        test.system.users.findOne();
    });

    assert.writeError(test.foo.save({_id: 0}));

    assert.throws(function () {
        test.foo.findOne({_id: 0});
    });

    assert.writeError(test.foo.update({_id: 0}, {$set: {x: 20}}));
    assert.writeError(test.foo.remove({_id: 0}));

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

    // Additional commands not permitted
    // Create non-admin user
    assert.throws(function () {
        mongo.getDB("test").createUser({user: username, pwd: password, roles: ["readWrite"]});
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
};

let authenticate = function (mongo) {
    print("============ authenticating user.");
    mongo.getDB("admin").auth(username, password);
};

let shutdown = function (conn) {
    print("============ shutting down.");
    MongoRunner.stopMongod(conn, /*signal*/ false, {auth: {user: username, pwd: password}});
};

let runTest = function (useHostName, useSession) {
    print("==========================");
    print("starting mongod: useHostName=" + useHostName);
    print("==========================");
    let conn = MongoRunner.runMongod({auth: "", dbpath: dbpath, useHostName: useHostName});

    let mongo = new Mongo("localhost:" + conn.port);

    assertCannotRunCommands(mongo);

    if (useSession) {
        let session = mongo.startSession();
        createUser(session.getDatabase("admin"));
        session.endSession();
    } else {
        createUser(mongo.getDB("admin"));
    }

    assertCannotRunCommands(mongo);

    authenticate(mongo);

    assertCanRunCommands(mongo);

    print("============ reconnecting with new client.");
    mongo = new Mongo("localhost:" + conn.port);

    assertCannotRunCommands(mongo);

    authenticate(mongo);

    assertCanRunCommands(mongo);

    shutdown(conn);
};

let runNonlocalTest = function (host) {
    print("==========================");
    print("starting mongod: non-local host access " + host);
    print("==========================");
    let conn = MongoRunner.runMongod({auth: "", dbpath: dbpath});

    let mongo = new Mongo(host + ":" + conn.port);

    assertCannotRunCommands(mongo);
    assert.throws(function () {
        mongo.getDB("admin").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
    });
    assert.throws(function () {
        mongo.getDB("$external").createUser({user: username, pwd: password, roles: jsTest.adminUserRoles});
    });
    shutdown(conn);
};

// Per SERVER-23503, the existence of roles in the admin database should disable the localhost
// exception.
// Start the server without auth. Create a role. Restart the server with auth. The exception is
// now enabled.
let runRoleTest = function () {
    let conn = MongoRunner.runMongod({dbpath: dbpath});
    let mongo = new Mongo("localhost:" + conn.port);
    assertCanRunCommands(mongo);
    createRole(mongo);
    assertCanRunCommands(mongo);
    MongoRunner.stopMongod(conn);
    conn = MongoRunner.runMongod({auth: "", dbpath: dbpath, restart: true, cleanData: false});
    mongo = new Mongo("localhost:" + conn.port);
    assertCannotRunCommands(mongo);
    MongoRunner.stopMongod(conn);
};

runTest(false, false);
runTest(false, true);
runTest(true, false);
runTest(true, true);

runNonlocalTest(get_ipaddr());

runRoleTest();
