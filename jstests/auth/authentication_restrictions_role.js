/**
 * This test checks that authentication restrictions can be set on roles and respected.
 * @tags: [requires_replication, requires_sharding]
 */
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Multiple users cannot be authenticated on one connection within a session.
TestData.disableImplicitSessions = true;

function testRestrictionCreationAndEnforcement(
    conn,
    eventuallyConsistentConn,
    sleepUntilUserDataPropagated,
    sleepUntilUserDataRefreshed,
) {
    // Create a session which observes an eventually consistent view of user data
    const eventualDb = eventuallyConsistentConn.getDB("admin");

    // Create a session for modifying user data during the life of the test
    const adminSession = new Mongo("127.0.0.1:" + conn.port);
    const admin = adminSession.getDB("admin");
    assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "admin", roles: [{role: "root", db: "admin"}]}));
    assert(admin.auth("admin", "admin"));

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
    const adminDB = adminMongo.getDB("admin");
    assert(adminDB.auth("admin", "admin"));

    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role2",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1/32"]}],
        }),
    );
    assert(Object.keys(adminDB.system.roles.findOne({role: "role2"})).includes("authenticationRestrictions"));
    assert.commandWorked(adminDB.runCommand({createRole: "role3", roles: [], privileges: []}));

    print("=== Role creation tests");
    print("When a role is updated, it retains authenticationRestrictions");
    assert.commandWorked(adminDB.runCommand({updateRole: "role2", roles: ["root"]}));
    const role2Info = assert.commandWorked(
        adminDB.runCommand({rolesInfo: "role2", showAuthenticationRestrictions: true}),
    );
    printjson(role2Info);
    assert.eq(
        JSON.stringify([[{clientSource: ["127.0.0.1/32"]}]]),
        JSON.stringify(role2Info.roles[0].authenticationRestrictions),
    );

    print(
        "When a client creates roles with empty authenticationRestrictions, the operation succeeds, though it has no effect",
    );
    assert.commandWorked(
        adminDB.runCommand({createRole: "role4", roles: [], privileges: [], authenticationRestrictions: []}),
    );
    assert(!Object.keys(adminDB.system.roles.findOne({role: "role4"})).includes("authenticationRestrictions"));

    print(
        "When a client updates a role's authenticationRestrictions to be empty, the operation succeeds, and removes the authenticationRestrictions field",
    );
    assert.commandWorked(adminDB.runCommand({createRole: "role5", roles: [], privileges: []}));
    assert.commandWorked(adminDB.runCommand({updateRole: "role5", authenticationRestrictions: []}));
    assert(!Object.keys(adminDB.system.roles.findOne({role: "role5"})).includes("authenticationRestrictions"));
    assert.commandWorked(
        adminDB.runCommand({updateRole: "role5", authenticationRestrictions: [{clientSource: ["127.0.0.1"]}]}),
    );
    assert(Object.keys(adminDB.system.roles.findOne({role: "role5"})).includes("authenticationRestrictions"));
    assert.commandWorked(adminDB.runCommand({updateRole: "role5", authenticationRestrictions: []}));
    assert(!Object.keys(adminDB.system.roles.findOne({role: "role5"})).includes("authenticationRestrictions"));

    print("When a client creates roles, it may use clientSource and serverAddress authenticationRestrictions");
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role6",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}],
        }),
    );
    assert(Object.keys(adminDB.system.roles.findOne({role: "role6"})).includes("authenticationRestrictions"));
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role7",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{serverAddress: ["127.0.0.1"]}],
        }),
    );
    assert(Object.keys(adminDB.system.roles.findOne({role: "role7"})).includes("authenticationRestrictions"));
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role8",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"], serverAddress: ["127.0.0.1"]}],
        }),
    );
    assert(Object.keys(adminDB.system.roles.findOne({role: "role8"})).includes("authenticationRestrictions"));
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role9",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}, {serverAddress: ["127.0.0.1"]}],
        }),
    );
    assert(Object.keys(adminDB.system.roles.findOne({role: "role9"})).includes("authenticationRestrictions"));
    assert.commandFailed(
        adminDB.runCommand({
            createRole: "role10",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{invalidRestriction: ["127.0.0.1"]}],
        }),
    );

    print("=== Localhost access tests");
    print('When a client on the loopback authenticates to a user with {clientSource: "127.0.0.1"}, it will succeed');
    assert.commandWorked(adminDB.runCommand({createUser: "user6", pwd: "user", roles: ["role6"]}));
    assert(db.auth("user6", "user"));
    db.logout();

    print('When a client on the loopback authenticates to a user with {serverAddress: "127.0.0.1"}, it will succeed');
    assert.commandWorked(adminDB.runCommand({createUser: "user7", pwd: "user", roles: ["role7"]}));
    assert(db.auth("user7", "user"));
    db.logout();

    print(
        'When a client on the loopback authenticates to a user with {clientSource: "127.0.0.1", serverAddress: "127.0.0.1"}, it will succeed',
    );
    assert.commandWorked(adminDB.runCommand({createUser: "user8", pwd: "user", roles: ["role8"]}));
    assert(db.auth("user8", "user"));
    db.logout();

    print("=== Remote access tests");
    print(
        'When a client on the external interface authenticates to a user with {clientSource: "127.0.0.1"}, it will fail',
    );
    assert(!externalDb.auth("user6", "user"));

    print(
        'When a client on the external interface authenticates to a user with {serverAddress: "127.0.0.1"}, it will fail',
    );
    assert(!externalDb.auth("user7", "user"));

    print(
        'When a client on the external interface authenticates to a user with {clientSource: "127.0.0.1", serverAddress: "127.0.0.1"}, it will fail',
    );
    assert(!externalDb.auth("user8", "user"));

    print("=== Invalidation tests");
    print("When a client removes all authenticationRestrictions from a role, authentication will succeed");
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role11",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"], serverAddress: ["127.0.0.1"]}],
        }),
    );
    assert.commandWorked(adminDB.runCommand({createUser: "user11", pwd: "user", roles: ["role11"]}));
    assert(!externalDb.auth("user11", "user"));
    assert.commandWorked(adminDB.runCommand({updateRole: "role11", authenticationRestrictions: []}));
    assert(externalDb.auth("user11", "user"));
    externalDb.logout();

    print("When a client sets authenticationRestrictions on a role, authorization privileges are revoked");
    assert.commandWorked(
        adminDB.runCommand({
            createRole: "role12",
            roles: [],
            privileges: [{resource: {db: "test", collection: "foo"}, actions: ["find"]}],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}],
        }),
    );
    assert.commandWorked(adminDB.runCommand({createUser: "user12", pwd: "user", roles: ["role12"]}));
    assert(db.auth("user12", "user"));
    assert.commandWorked(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
    db.logout();

    sleepUntilUserDataPropagated();

    assert(eventualDb.auth("user12", "user"));
    assert.commandWorked(eventualDb.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
    assert.commandWorked(
        adminDB.runCommand({updateRole: "role12", authenticationRestrictions: [{clientSource: ["192.168.2.0"]}]}),
    );
    assert.commandFailed(db.getSiblingDB("test").runCommand({find: "foo", batchSize: 0}));
    eventualDb.logout();

    sleepUntilUserDataRefreshed();
    assert(!eventualDb.auth("user12", "user"));
}

function testUsersInfoCommand(conn) {
    function forEachUser(res, assertFun) {
        assert(res.hasOwnProperty("users"));
        print("Users: " + tojson(res.users));
        assert.gt(res.users.length, 0);
        res.users.forEach(assertFun);
    }

    const admin = conn.getDB("admin");
    assert(admin.auth("admin", "admin"));

    assert.commandWorked(admin.runCommand({createUser: "user", pwd: "pwd", roles: []}));
    assert.commandWorked(
        admin.runCommand({
            createUser: "restrictedUser",
            pwd: "pwd",
            roles: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}],
        }),
    );
    assert.commandWorked(
        admin.runCommand({
            createRole: "restrictedRole",
            roles: [],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.2"]}],
        }),
    );
    assert.commandWorked(
        admin.runCommand({createUser: "userWithRestrictedRole", pwd: "pwd", roles: ["restrictedRole"]}),
    );
    assert.commandWorked(
        admin.runCommand({
            createUser: "restrictedUserWithRestrictedRole",
            pwd: "pwd",
            roles: ["restrictedRole"],
            authenticationRestrictions: [{clientSource: ["127.0.0.1"]}],
        }),
    );

    print("Calling usersInfo for all users on a database with showAuthenticationRestrictions is an error");
    assert.commandFailed(admin.runCommand({usersInfo: 1, showAuthenticationRestrictions: true}));

    print(
        "Calling usersInfo for all users on a database with showAuthenticationRestrictions false or unset will succeed, and not produce authenticationRestriction fields",
    );
    [{}, {showAuthenticationRestrictions: false}].forEach(function (fragment) {
        forEachUser(assert.commandWorked(admin.runCommand(Object.merge({usersInfo: 1}, fragment))), function (userDoc) {
            assert(!userDoc.hasOwnProperty("authenticationRestrictions"));
            assert(!userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
        });
    });

    print(
        "If usersInfo is called with showAuthenticationRestrictions true, on a user without authenticationRestrictions, a document with empty authenticationRestrictions and inheritedAuthenticationRestrictions arrays is returned",
    );
    forEachUser(
        assert.commandWorked(admin.runCommand({usersInfo: "user", showAuthenticationRestrictions: true})),
        function (userDoc) {
            assert(userDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(0, userDoc["authenticationRestrictions"].length);

            assert(userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(0, userDoc["inheritedAuthenticationRestrictions"].length);
        },
    );

    print(
        "If usersInfo is called and showAuthenticationRestrictions is false or unset, return a document without an authenticationRestrictions or inheritedAuthenticationRestrictions field",
    );
    ["user", "restrictedUser", "userWithRestrictedRole", "restrictedUserWithRestrictedRole"].forEach(function (user) {
        forEachUser(
            assert.commandWorked(admin.runCommand({usersInfo: "user", showAuthenticationRestrictions: false})),
            function (userDoc) {
                assert(!userDoc.hasOwnProperty("authenticationRestrictions"));
                assert(!userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            },
        );
        forEachUser(assert.commandWorked(admin.runCommand({usersInfo: "user"})), function (userDoc) {
            assert(!userDoc.hasOwnProperty("authenticationRestrictions"));
            assert(!userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
        });
    });

    print("Authentication restrictions can be obtained through usersInfo for a single user with restrictions");
    forEachUser(
        assert.commandWorked(admin.runCommand({usersInfo: "restrictedUser", showAuthenticationRestrictions: true})),
        function (userDoc) {
            assert(userDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(1, userDoc["authenticationRestrictions"].length);

            assert(userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(0, userDoc["inheritedAuthenticationRestrictions"].length);
        },
    );

    print("Authentication restrictions can be obtained through usersInfo for a single user with restrictioned roles");
    forEachUser(
        assert.commandWorked(
            admin.runCommand({usersInfo: "userWithRestrictedRole", showAuthenticationRestrictions: true}),
        ),
        function (userDoc) {
            assert(userDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(0, userDoc["authenticationRestrictions"].length);

            assert(userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(1, userDoc["inheritedAuthenticationRestrictions"].length);
        },
    );

    print(
        "Authentication restrictions can be obtained through usersInfo for a single restricted user with restrictioned roles",
    );
    forEachUser(
        assert.commandWorked(
            admin.runCommand({usersInfo: "restrictedUserWithRestrictedRole", showAuthenticationRestrictions: true}),
        ),
        function (userDoc) {
            print("This doc: " + tojson(userDoc));
            assert(userDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(1, userDoc["authenticationRestrictions"].length);

            assert(userDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(1, userDoc["inheritedAuthenticationRestrictions"].length);
        },
    );

    admin.logout();
}

function testRolesInfoCommand(conn) {
    function forEachRole(res, assertFun) {
        assert(res.hasOwnProperty("roles"));
        print("Users: " + tojson(res.roles));
        assert.gt(res.roles.length, 0);
        res.roles.forEach(assertFun);
    }

    const admin = conn.getDB("admin");
    assert(admin.auth("admin", "admin"));

    assert.commandWorked(admin.runCommand({createRole: "role", roles: [], privileges: []}));
    // restrictedRole already created
    assert.commandWorked(
        admin.runCommand({createRole: "roleWithRestrictedRole", roles: ["restrictedRole"], privileges: []}),
    );
    assert.commandWorked(
        admin.runCommand({
            createRole: "restrictedRoleWithRestrictedRole",
            roles: ["restrictedRole"],
            privileges: [],
            authenticationRestrictions: [{clientSource: ["127.0.0.3"]}],
        }),
    );

    ["role", "restrictedRole", "roleWithRestrictedRole", "restrictedRoleWithRestrictedRole"].forEach(function (role) {
        forEachRole(assert.commandWorked(admin.runCommand({rolesInfo: role})), function (roleDoc) {
            assert(!roleDoc.hasOwnProperty("authenticationRestrictions"));
            assert(!roleDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
        });
    });

    forEachRole(
        assert.commandWorked(admin.runCommand({rolesInfo: "role", showAuthenticationRestrictions: true})),
        function (roleDoc) {
            assert(roleDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(0, roleDoc.authenticationRestrictions.length);
            assert(roleDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(0, roleDoc.inheritedAuthenticationRestrictions.length);
        },
    );

    forEachRole(
        assert.commandWorked(admin.runCommand({rolesInfo: "restrictedRole", showAuthenticationRestrictions: true})),
        function (roleDoc) {
            assert(roleDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(1, roleDoc.authenticationRestrictions.length);
            assert(roleDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(1, roleDoc.inheritedAuthenticationRestrictions.length);
        },
    );

    forEachRole(
        assert.commandWorked(
            admin.runCommand({rolesInfo: "roleWithRestrictedRole", showAuthenticationRestrictions: true}),
        ),
        function (roleDoc) {
            assert(roleDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(0, roleDoc.authenticationRestrictions.length);
            assert(roleDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(1, roleDoc.inheritedAuthenticationRestrictions.length);
        },
    );

    forEachRole(
        assert.commandWorked(
            admin.runCommand({rolesInfo: "restrictedRoleWithRestrictedRole", showAuthenticationRestrictions: true}),
        ),
        function (roleDoc) {
            assert(roleDoc.hasOwnProperty("authenticationRestrictions"));
            assert.eq(1, roleDoc.authenticationRestrictions.length);
            assert(roleDoc.hasOwnProperty("inheritedAuthenticationRestrictions"));
            assert.eq(2, roleDoc.inheritedAuthenticationRestrictions.length);
        },
    );

    admin.logout();
}

const keyfile = "jstests/libs/key1";

print("Testing standalone");
const conn = MongoRunner.runMongod({bind_ip_all: "", auth: ""});
testRestrictionCreationAndEnforcement(
    conn,
    conn,
    function () {},
    function () {},
);
testUsersInfoCommand(conn);
testRolesInfoCommand(conn);
MongoRunner.stopMongod(conn);

print("Testing replicaset");
const rst = new ReplSetTest({name: "testset", nodes: 2, nodeOptions: {bind_ip_all: "", auth: ""}, keyFile: keyfile});
const nodes = rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();
const awaitReplication = function () {
    authutil.asCluster(nodes, "jstests/libs/key1", function () {
        rst.awaitReplication();
    });
};

testRestrictionCreationAndEnforcement(rst.getPrimary(), rst.getSecondary(), awaitReplication, awaitReplication);
testUsersInfoCommand(rst.getPrimary());
testRolesInfoCommand(rst.getPrimary());
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
        rsOptions: {auth: null},
    },
});
testRestrictionCreationAndEnforcement(
    st.s0,
    st.s1,
    function () {},
    function () {
        sleep(40 * 1000); // Wait for mongos user cache invalidation
    },
);
testUsersInfoCommand(st.s0);
st.stop();
