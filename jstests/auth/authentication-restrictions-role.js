/**
 * This test checks that authentication restrictions can be set on roles and respected.
 */

(function() {
    'use strict';

    function testConnection(
        conn, eventuallyConsistentConn, sleepUntilUserDataPropagated, sleepUntilUserDataRefreshed) {
        load("jstests/libs/host_ipaddr.js");

        // Create a session which observes an eventually consistent view of user data
        var eventualDb = eventuallyConsistentConn.getDB("admin");

        // Create a session for modifying user data during the life of the test
        var adminSession = new Mongo("127.0.0.1:" + conn.port);
        var admin = adminSession.getDB("admin");
        assert.commandWorked(admin.runCommand(
            {createUser: "admin", pwd: "admin", roles: [{role: "root", db: "admin"}]}));
        assert(admin.auth("admin", "admin"));

        // Create a strongly consistent session for consuming user data
        var db = conn.getDB("admin");

        // Create a strongly consistent session for consuming user data, with a non-localhost
        // source IP.
        var externalMongo = new Mongo(get_ipaddr() + ":" + conn.port);
        var externalDb = externalMongo.getDB("admin");

        print("=== Feature compatibility tests");
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "3.4"}));

        print(
            "Given a server with 3.4 featureCompatibilityVersions, it is not possible to make a role with authenticationRestrictions");
        assert.commandFailed(admin.runCommand({
            createRole: "role1",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
        }));
        assert.commandWorked(admin.runCommand({createRole: "role1", roles: [], privileges: []}));

        print(
            "When the server is upgraded to 3.6 featureCompatibilityVersion, roles may be created with authenticationRestrictions");
        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "3.6"}));
        assert.commandWorked(admin.runCommand({
            createRole: "role2",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
        }));
        assert(Object.keys(admin.system.roles.findOne({role: "role2"}))
                   .includes("authenticationRestrictions"));
        assert.commandWorked(admin.runCommand({createRole: "role3", roles: [], privileges: []}));

        print("=== Role creation tests");
        print(
            "When a client creates roles with empty authenticationRestrictions, the operation succeeds, though it has no effect");
        assert.commandWorked(admin.runCommand(
            {createRole: "role4", roles: [], privileges: [], authenticationRestrictions: []}));
        assert(!Object.keys(admin.system.roles.findOne({role: "role4"}))
                    .includes("authenticationRestrictions"));

        print(
            "When a client updates a role's authenticationRestrictions to be empty, the operation succeeds, and removes the authenticationRestrictions field");
        assert.commandWorked(admin.runCommand({createRole: "role5", roles: [], privileges: []}));
        assert.commandWorked(
            admin.runCommand({updateRole: "role5", authenticationRestrictions: []}));
        assert(!Object.keys(admin.system.roles.findOne({role: "role5"}))
                    .includes("authenticationRestrictions"));
        assert.commandWorked(admin.runCommand(
            {updateRole: "role5", authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]}));
        assert(Object.keys(admin.system.roles.findOne({role: "role5"}))
                   .includes("authenticationRestrictions"));
        assert.commandWorked(
            admin.runCommand({updateRole: "role5", authenticationRestrictions: []}));
        assert(!Object.keys(admin.system.roles.findOne({role: "role5"}))
                    .includes("authenticationRestrictions"));

        print(
            "When a client creates roles, it may use clientSource and serverAddress authenticationRestrictions");
        assert.commandWorked(admin.runCommand({
            createRole: "role6",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
        }));
        assert(Object.keys(admin.system.roles.findOne({role: "role6"}))
                   .includes("authenticationRestrictions"));
        assert.commandWorked(admin.runCommand({
            createRole: "role7",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{serverAddress: ["127.0.0.1"]}]
        }));
        assert(Object.keys(admin.system.roles.findOne({role: "role7"}))
                   .includes("authenticationRestrictions"));
        assert.commandWorked(admin.runCommand({
            createRole: "role8",
            roles: [],
            privileges: [],
            authenticationRestrictions:
                [{clientSource: ["127.0.0.1"], serverAddress: ["127.0.0.1"]}]
        }));
        assert(Object.keys(admin.system.roles.findOne({role: "role8"}))
                   .includes("authenticationRestrictions"));
        assert.commandWorked(admin.runCommand({
            createRole: "role9",
            roles: [],
            privileges: [],
            authenticationRestrictions:
                [{clientSource: ["127.0.0.1"]}, {serverAddress: ["127.0.0.1"]}]
        }));
        assert(Object.keys(admin.system.roles.findOne({role: "role9"}))
                   .includes("authenticationRestrictions"));
        assert.commandFailed(admin.runCommand({
            createRole: "role10",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{invalidRestriction: ["127.0.0.1"]}]
        }));

        print("=== Localhost access tests");
        print(
            "When a client on the loopback authenticates to a user with {clientSource: \"127.0.0.1\"}, it will succeed");
        assert.commandWorked(
            admin.runCommand({createUser: "user6", pwd: "user", roles: ["role6"]}));
        assert(db.auth("user6", "user"));

        print(
            "When a client on the loopback authenticates to a user with {serverAddress: \"127.0.0.1\"}, it will succeed");
        assert.commandWorked(
            admin.runCommand({createUser: "user7", pwd: "user", roles: ["role7"]}));
        assert(db.auth("user7", "user"));

        print(
            "When a client on the loopback authenticates to a user with {clientSource: \"127.0.0.1\", serverAddress: \"127.0.0.1\"}, it will succeed");
        assert.commandWorked(
            admin.runCommand({createUser: "user8", pwd: "user", roles: ["role8"]}));
        assert(db.auth("user8", "user"));

        print("=== Remote access tests");
        print(
            "When a client on the external interface authenticates to a user with {clientSource: \"127.0.0.1\"}, it will fail");
        assert(!externalDb.auth("user6", "user"));

        print(
            "When a client on the external interface authenticates to a user with {serverAddress: \"127.0.0.1\"}, it will fail");
        assert(!externalDb.auth("user7", "user"));

        print(
            "When a client on the external interface authenticates to a user with {clientSource: \"127.0.0.1\", serverAddress: \"127.0.0.1\"}, it will fail");
        assert(!externalDb.auth("user8", "user"));

        print("=== Invalidation tests");
        print(
            "When a client removes all authenticationRestrictions from a role, authentication will succeed");
        assert.commandWorked(admin.runCommand({
            createRole: "role11",
            roles: [],
            privileges: [],
            authenticationRestrictions:
                [{clientSource: ["127.0.0.1"], serverAddress: ["127.0.0.1"]}]
        }));
        assert.commandWorked(
            admin.runCommand({createUser: "user11", pwd: "user", roles: ["role11"]}));
        assert(!externalDb.auth("user11", "user"));
        assert.commandWorked(
            admin.runCommand({updateRole: "role11", authenticationRestrictions: []}));
        assert(externalDb.auth("user11", "user"));

        print(
            "When a client sets authenticationRestrictions on a role, authorization privileges are revoked");
        assert.commandWorked(admin.runCommand({
            createRole: "role12",
            roles: [],
            privileges: [{resource: {db: "test", collection: "foo"}, actions: ["find"]}],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
        }));
        assert.commandWorked(
            admin.runCommand({createUser: "user12", pwd: "user", roles: ["role12"]}));
        assert(db.auth("user12", "user"));
        assert.commandWorked(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
        sleepUntilUserDataPropagated();
        assert(eventualDb.auth("user12", "user"));
        assert.commandWorked(
            eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
        assert.commandWorked(admin.runCommand(
            {updateRole: "role12", authenticationRestrictions: [{clientSource: ["192.168.2.0"]}]}));
        assert.commandFailed(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
        sleepUntilUserDataRefreshed();
        assert.commandFailed(
            eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));

        print(
            "When a client downgrades featureCompatibilityVersion, roles with featureCompatibilityVersions become unusable.");
        assert.commandWorked(admin.runCommand({
            createRole: "role13",
            roles: [],
            privileges: [{resource: {db: "test", collection: "foo"}, actions: ["find"]}],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
        }));
        assert.commandWorked(
            admin.runCommand({createUser: "user13", pwd: "user", roles: ["role13"]}));
        assert(db.auth("user13", "user"));
        assert.commandWorked(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));

        sleepUntilUserDataPropagated();
        assert(eventualDb.auth("user13", "user"));
        assert.commandWorked(
            eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));

        assert.commandWorked(admin.runCommand({setFeatureCompatibilityVersion: "3.4"}));

        sleepUntilUserDataRefreshed();
        assert.commandFailed(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
        assert.commandFailed(
            eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
    }

    print("Testing standalone");
    var conn = MongoRunner.runMongod({bind_ip_all: "", auth: ""});
    testConnection(conn, conn, function() {}, function() {});
    MongoRunner.stopMongod(conn);

    var keyfile = "jstests/libs/key1";

    print("Testing replicaset");
    var rst = new ReplSetTest(
        {name: 'testset', nodes: 2, nodeOptions: {bind_ip_all: "", auth: ""}, keyFile: keyfile});
    var nodes = rst.startSet();
    rst.initiate();
    rst.awaitSecondaryNodes();
    var awaitReplication = function() {
        authutil.asCluster(nodes, "jstests/libs/key1", function() {
            rst.awaitReplication();
        });
    };

    testConnection(rst.getPrimary(), rst.getSecondary(), awaitReplication, awaitReplication);
    rst.stopSet();

    print("Testing sharded cluster");
    var st = new ShardingTest({
        mongos: 2,
        config: 3,
        shard: 1,
        keyFile: keyfile,
        other: {
            mongosOptions: {bind_ip_all: "", auth: null},
            configOptions: {auth: null},
            shardOptions: {auth: null}
        }
    });
    testConnection(st.s0,
                   st.s1,
                   function() {},
                   function() {
                       sleep(40 * 1000);  // Wait for mongos user cache invalidation
                   });
    st.stop();

}());
