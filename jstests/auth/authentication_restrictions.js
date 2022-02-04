/**
 * This test checks that authentication restrictions can be set and respected.
 * @tags: [requires_sharding, requires_replication]
 */

(function() {
'use strict';

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

function testConnection(
    conn, eventuallyConsistentConn, sleepUntilUserDataPropagated, sleepUntilUserDataRefreshed) {
    load("jstests/libs/host_ipaddr.js");

    // Create a session which observes an eventually consistent view of user data
    const eventualDb = eventuallyConsistentConn.getDB("admin");

    // Create a session for modifying user data during the life of the test
    const adminSession = new Mongo("localhost:" + conn.port);
    const admin = adminSession.getDB("admin");
    assert.commandWorked(admin.runCommand(
        {createUser: "admin", pwd: "admin", roles: [{role: "root", db: "admin"}]}));
    assert(admin.auth("admin", "admin"));
    admin.logout();

    // Create a strongly consistent session for consuming user data
    const db = conn.getDB("admin");

    // Create a strongly consistent session for consuming user data, with a non-localhost
    // source IP.
    const externalMongo = new Mongo(get_ipaddr() + ":" + conn.port);
    const externalDb = externalMongo.getDB("admin");

    // Create a connection which remains authenticated as 'admin'
    // so that we can create/mutate users/roles while we do
    // multiple authentications.
    const adminMongo = new Mongo(conn.host);
    const adminDB = adminMongo.getDB('admin');
    assert(adminDB.auth('admin', 'admin'));

    assert.commandWorked(adminDB.runCommand({
        createUser: "user2",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
    }));
    assert.commandWorked(adminDB.runCommand({createUser: "user3", pwd: "user", roles: []}));
    assert.commandWorked(adminDB.runCommand(
        {updateUser: "user3", authenticationRestrictions: [{serverAddress: ["127.0.0.1"]}]}));

    print("=== User creation tests");
    print(
        "When a client creates users with empty authenticationRestrictions, the operation succeeds, though it has no effect");
    assert.commandWorked(adminDB.runCommand(
        {createUser: "user4", pwd: "user", roles: [], authenticationRestrictions: []}));
    assert(!Object.keys(adminDB.system.users.findOne({user: "user4"}))
                .includes("authenticationRestrictions"));

    print(
        "When a client updates a user's authenticationRestrictions to be empty, the operation succeeds, and removes the authenticationRestrictions field");
    assert.commandWorked(adminDB.runCommand({createUser: "user5", pwd: "user", roles: []}));
    assert.commandWorked(adminDB.runCommand({updateUser: "user5", authenticationRestrictions: []}));
    assert(!Object.keys(adminDB.system.users.findOne({user: "user5"}))
                .includes("authenticationRestrictions"));
    assert.commandWorked(adminDB.runCommand(
        {updateUser: "user5", authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]}));
    assert(Object.keys(adminDB.system.users.findOne({user: "user5"}))
               .includes("authenticationRestrictions"));
    assert.commandWorked(adminDB.runCommand({updateUser: "user5", authenticationRestrictions: []}));
    assert(!Object.keys(adminDB.system.users.findOne({user: "user5"}))
                .includes("authenticationRestrictions"));

    print(
        "When a client updates a user's authenticationRestrictions to be null or undefined, the operation fails");
    assert.commandWorked(adminDB.runCommand(
        {updateUser: "user5", authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]}));
    assert(Object.keys(adminDB.system.users.findOne({user: "user5"}))
               .includes("authenticationRestrictions"));
    assert.commandFailed(
        adminDB.runCommand({updateUser: "user5", authenticationRestrictions: null}));
    assert(Object.keys(adminDB.system.users.findOne({user: "user5"}))
               .includes("authenticationRestrictions"));
    assert.commandFailed(
        adminDB.runCommand({updateUser: "user5", authenticationRestrictions: undefined}));
    assert(Object.keys(adminDB.system.users.findOne({user: "user5"}))
               .includes("authenticationRestrictions"));

    print(
        "When a client creates users, it may use clientSource and serverAddress authenticationRestrictions");
    assert.commandWorked(adminDB.runCommand({
        createUser: "user6",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]
    }));
    assert.commandWorked(adminDB.runCommand({
        createUser: "user7",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{serverAddress: ["127.0.0.1"]}]
    }));
    assert.commandWorked(adminDB.runCommand({
        createUser: "user8",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{clientSource: ["127.0.0.1"], serverAddress: ["127.0.0.1"]}]
    }));
    assert.commandWorked(adminDB.runCommand({
        createUser: "user9",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{clientSource: ["127.0.0.1"]}, {serverAddress: ["127.0.0.1"]}]
    }));
    assert.commandFailed(adminDB.runCommand({
        createUser: "user10",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{invalidRestriction: ["127.0.0.1"]}]
    }));

    print("=== Localhost access tests");

    print(
        "When a client on the loopback authenticates to a user with {clientSource: \"127.0.0.1\"}, it will succeed");
    assert(db.auth("user6", "user"));
    db.logout();

    print(
        "When a client on the loopback authenticates to a user with {serverAddress: \"127.0.0.1\"}, it will succeed");
    assert(db.auth("user7", "user"));
    db.logout();

    print(
        "When a client on the loopback authenticates to a user with {clientSource: \"127.0.0.1\", serverAddress: \"127.0.0.1\"}, it will succeed");
    assert(db.auth("user8", "user"));
    db.logout();

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
        "When a client removes all authenticationRestrictions from a user, authentication will succeed");
    assert.commandWorked(adminDB.runCommand({
        createUser: "user11",
        pwd: "user",
        roles: [],
        authenticationRestrictions: [{clientSource: ["127.0.0.1"], serverAddress: ["127.0.0.1"]}]
    }));
    assert(!externalDb.auth("user11", "user"));
    assert.commandWorked(
        adminDB.runCommand({updateUser: "user11", authenticationRestrictions: []}));
    assert(externalDb.auth("user11", "user"));
    externalDb.logout();

    print(
        "When a client sets authenticationRestrictions on a user, authorization privileges are revoked");
    assert.commandWorked(adminDB.runCommand(
        {createUser: "user12", pwd: "user", roles: [{role: "readWrite", db: "test"}]}));

    assert(db.auth("user12", "user"));
    assert.commandWorked(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
    db.logout();

    sleepUntilUserDataPropagated();
    assert(eventualDb.auth("user12", "user"));
    assert.commandWorked(eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));

    assert.commandWorked(adminDB.runCommand(
        {updateUser: "user12", authenticationRestrictions: [{clientSource: ["192.0.2.0"]}]}));

    assert(!db.auth('user12', 'user'));

    sleepUntilUserDataRefreshed();
    assert.commandFailed(eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
}

print("Testing standalone");
const conn = MongoRunner.runMongod({bind_ip_all: "", auth: ""});
testConnection(conn, conn, function() {}, function() {});
MongoRunner.stopMongod(conn);

const keyfile = "jstests/libs/key1";

print("Testing replicaset");
const rst = new ReplSetTest(
    {name: 'testset', nodes: 2, nodeOptions: {bind_ip_all: "", auth: ""}, keyFile: keyfile});
const nodes = rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();
const awaitReplication = function() {
    authutil.asCluster(nodes, "jstests/libs/key1", function() {
        rst.awaitReplication();
    });
};

testConnection(rst.getPrimary(), rst.getSecondary(), awaitReplication, awaitReplication);
rst.stopSet();

print("Testing sharded cluster");
const st = new ShardingTest({
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
