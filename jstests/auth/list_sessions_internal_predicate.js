/**
 * Regression test for an authorization bypass in $listSessions via $_internalPredicate.
 *
 * The $_internalPredicate field is an internal serialization mechanism used by mongos to
 * forward an optimized $listSessions + $match pipeline to shard mongod instances. It must
 * not be accepted from external clients, because it short-circuits the logic that
 * restricts $listSessions results to the authenticated user's own sessions.
 *
 * @tags: [requires_auth]
 */

TestData.disableImplicitSessions = true;

const predicate = {
    "_id.uid": {$exists: true},
};

function assertBypassRejected(targetDB, collOrOne, pipeline) {
    const res = targetDB.runCommand({aggregate: collOrOne, pipeline, cursor: {}});
    assert.commandFailedWithCode(res, ErrorCodes.Unauthorized, {res, pipeline});
}

const mongod = MongoRunner.runMongod({auth: ""});
const admin = mongod.getDB("admin");
const config = mongod.getDB("config");

assert.commandWorked(admin.runCommand({createUser: "admin", pwd: "pass", roles: jsTest.adminUserRoles}));
assert(admin.auth("admin", "pass"));
assert.commandWorked(admin.runCommand({createUser: "attacker", pwd: "pass", roles: []}));

// Log in as an unprivileged external client.
admin.logout();
assert(admin.auth("attacker", "pass"));

// Sanity check: an unprivileged user cannot list all users' sessions.
assert.commandFailedWithCode(
    config.runCommand({aggregate: "system.sessions", pipeline: [{$listSessions: {allUsers: true}}], cursor: {}}),
    ErrorCodes.Unauthorized,
);

// Sanity check: an unprivileged user can list their own sessions without specifying $_internalPredicate.
assert.commandWorked(
    config.runCommand({
        aggregate: "system.sessions",
        pipeline: [{$listSessions: {users: [{user: "attacker", db: "admin"}]}}],
        cursor: {},
    }),
);

// Bypass with 'users' + $_internalPredicate. Without the fix this replaces the
// user-scoped match filter and exposes other users' sessions.
assertBypassRejected(config, "system.sessions", [
    {
        $listSessions: {
            users: [{user: "attacker", db: "admin"}],
            $_internalPredicate: predicate,
        },
    },
]);

// Bypass with just $_internalPredicate.
assertBypassRejected(config, "system.sessions", [{$listSessions: {$_internalPredicate: predicate}}]);

// Log in as admin.
admin.logout();
assert(admin.auth("admin", "pass"));

// Sanity check: an admin with the listSessions privilege can list all users' sessions normally.
assert.commandWorked(
    config.runCommand({
        aggregate: "system.sessions",
        pipeline: [{$listSessions: {allUsers: true}}],
        cursor: {},
    }),
);

// Even with admin privileges, supplying $_internalPredicate directly is rejected.
assertBypassRejected(config, "system.sessions", [{$listSessions: {allUsers: true, $_internalPredicate: predicate}}]);

admin.logout();
MongoRunner.stopMongod(mongod);
