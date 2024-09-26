/**
 * Test that a sharded explain command with a value set for the lsid's uid field in the inner
 * command invocation will ignore the lsid field and succeed so long as the user is authorized to
 * run the command being explained. Also tests that a user can only set a lsid + uid in the the
 * top-level explain command if they are authorized to do so. Verifies the fix for CVE-138.
 *
 * @tags: [requires_sharding]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({keyFile: "jstests/libs/key1", shards: {rs0: {nodes: 2}}});
const mongosConn = st.s;

// Setup: create user roles.
const adminDB = mongosConn.getDB("admin");
adminDB.createUser({user: "admin", pwd: "admin", roles: ["root"]});
adminDB.auth("admin", "admin");

const testDB = mongosConn.getDB("test");
testDB.test.insert({a: 1});

testDB.createUser({user: "readOnlyUser", pwd: "user", roles: jsTest.readOnlyUserRoles});
testDB.createUser({user: "readWriteUser", pwd: "user", roles: jsTest.basicUserRoles});
adminDB.createRole({
    role: "impersonateRole",
    roles: [],
    privileges: [
        {resource: {db: "test", collection: "test"}, actions: ["find", "update"]},
        {resource: {cluster: true}, actions: ["impersonate"]}
    ]
});
adminDB.createUser({user: "impersonateUser", pwd: "user", roles: ["impersonateRole"]});
adminDB.logout();

// LSID helper functions.
const cmdLsid = UUID();
const invalidUid = computeSHA256Block("abc");
const getUserUid = function() {
    const user = adminDB.runCommand({connectionStatus: 1}).authInfo.authenticatedUsers[0];
    return user ? computeSHA256Block(user.user + "@" + user.db) : computeSHA256Block("");
};

const createLsidObj = (uid) => ({lsid: {id: cmdLsid, uid}});
const commandWithLsid = (cmd, lsidObj, isOuter = false) => {
    return isOuter ? {explain: cmd, ...lsidObj} : {explain: {...cmd, ...lsidObj}};
};

// Explain command helper and runners.
const assertExplain = (cmd, uid, shouldFail = false, isOuter = false) => {
    const lsidObj = createLsidObj(uid);
    const command = commandWithLsid(cmd, lsidObj, isOuter);

    shouldFail ? assert.commandFailedWithCode(testDB.runCommand(command), ErrorCodes.Unauthorized)
               : assert.commandWorked(testDB.runCommand(command));
};

const assertInnerLsidExplainIsUnauthorized = (cmd, uid) => assertExplain(cmd, uid, true);
const assertInnerLsidExplainWorked = (cmd, uid) => assertExplain(cmd, uid, false);
const assertTopLevelLsidExplainIsUnauthorized = (cmd, uid) => assertExplain(cmd, uid, true, true);
const assertTopLevelLsidExplainWorked = (cmd, uid) => assertExplain(cmd, uid, false, true);

const runCommandWithLsidTest =
    (isAuthZforInnerCmd, canSetTopLevelLsid, cmd, invalidUid, userUid) => {
        // A user can put any arbitrary lsid/generic argument in the inner command invocation, as
        // this will be pruned when the command is wrapped into an explain command. The explain will
        // succeed so long as the user is authorized to run the command being explained. A user can
        // only specify an lsid in the top-level explain command if they are authorized to do so, or
        // if the uid in the lsid is the same as the user digest in the opCtx. The explain command
        // will succeed if the user is authorized to run the command being explained.
        if (isAuthZforInnerCmd) {
            assertInnerLsidExplainWorked(cmd, invalidUid);
            assertInnerLsidExplainWorked(cmd, userUid);
            assertTopLevelLsidExplainWorked(cmd, userUid);
        } else {
            assertInnerLsidExplainIsUnauthorized(cmd, invalidUid);
            assertInnerLsidExplainIsUnauthorized(cmd, userUid);
            assertTopLevelLsidExplainIsUnauthorized(cmd, userUid);
        }

        canSetTopLevelLsid ? assertTopLevelLsidExplainWorked(cmd, invalidUid)
                           : assertTopLevelLsidExplainIsUnauthorized(cmd, invalidUid);
    };

// Commands to run explain on.
const commands = [
    {cmd: {distinct: "test", key: "_id"}, isWrite: false},
    {cmd: {count: "test"}, isWrite: false},
    {cmd: {find: "test", filter: {a: 1}}, isWrite: false},
    {
        cmd: {findAndModify: "test", query: {a: 1}, update: {$set: {b: 2}}, "new": true},
        isWrite: true
    },
    {cmd: {update: "test", updates: [{q: {a: 1}, u: {c: "foo"}}]}, isWrite: true}
];

// A user is only authorized to run an explain on a command if they:
// 1. Are authorized to run the command being explained on the associated resource.
// 2. Are authorized to set any generic arguments attached to the top-level invocation of the
// explain. In particular, a user can only set a different value in the uid field of an lsid if they
// have the "impersonate" role. If a user specifies the same uid value as their user digest, the
// command will succeed if (1) is true.
const users = [
    {user: "readOnlyUser", pwd: "user", authDB: testDB, canWrite: false, canSetTopLevelLsid: false},
    {user: "readWriteUser", pwd: "user", authDB: testDB, canWrite: true, canSetTopLevelLsid: false},
    {
        user: "impersonateUser",
        pwd: "user",
        authDB: adminDB,
        canWrite: true,
        canSetTopLevelLsid: true
    },
    {user: "admin", pwd: "admin", authDB: adminDB, canWrite: true, canSetTopLevelLsid: false},
];

users.forEach(({user, pwd, authDB, canWrite, canSetTopLevelLsid}) => {
    authDB.auth(user, pwd);
    const userUid = getUserUid();

    commands.forEach(({cmd, isWrite}) => {
        const isAuthZforInnerCmd = canWrite || !isWrite;
        runCommandWithLsidTest(isAuthZforInnerCmd, canSetTopLevelLsid, cmd, invalidUid, userUid);
    });

    authDB.logout();
});

st.stop();
