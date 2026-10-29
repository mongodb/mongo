/**
 * Verifies mismatching cluster time objects are rejected by a sharded cluster when auth is on. In
 * noPassthrough because auth is manually set.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Given a valid cluster time object, returns one with the same signature, but a mismatching
// cluster time.
function mismatchingLogicalTime(lt) {
    return Object.merge(lt, {clusterTime: Timestamp(lt.clusterTime.getTime() + 100, 0)});
}

function assertRejectsMismatchingLogicalTime(db) {
    let validTime = db.runCommand({hello: 1}).$clusterTime;
    let mismatchingTime = mismatchingLogicalTime(validTime);

    assert.commandFailedWithCode(
        db.runCommand({hello: 1, $clusterTime: mismatchingTime}),
        ErrorCodes.TimeProofMismatch,
        "expected command with mismatching cluster time and signature to be rejected",
    );
}

function assertAcceptsValidLogicalTime(db) {
    let validTime = db.runCommand({hello: 1}).$clusterTime;
    assert.commandWorked(
        db.runCommand({hello: 1, $clusterTime: validTime}),
        "expected command with valid cluster time and signature to be accepted",
    );
}

// Unauthenticated clients without the localhost auth bypass skip signature validation to avoid
// exposing a signing-key oracle.
function assertIgnoresMismatchingLogicalTimeWhenUnauthenticated(db) {
    const authInfo = assert.commandWorked(db.runCommand({connectionStatus: 1})).authInfo;
    assert.eq([], authInfo.authenticatedUsers, "expected an unauthenticated connection");

    let mismatchingTime = mismatchingLogicalTime(db.runCommand({hello: 1}).$clusterTime);

    assert.commandWorked(
        db.runCommand({hello: 1, $clusterTime: mismatchingTime}),
        "expected command with mismatching cluster time and signature from an unauthenticated " +
            "client to be silently ignored rather than rejected",
    );

    // Auth-required commands are rejected before request metadata is read, so the signature is
    // never validated.
    assert.commandFailedWithCode(
        db.runCommand({find: "foo", $clusterTime: mismatchingTime}),
        ErrorCodes.Unauthorized,
        "expected an unauthenticated command requiring authentication to fail with Unauthorized " +
            "rather than reporting the result of validating the cluster time signature",
    );
}

// Authenticated clients' cluster times are validated before the command authorization check.
function assertValidatesBeforeAuthorizationCheck(db) {
    let mismatchingTime = mismatchingLogicalTime(db.runCommand({hello: 1}).$clusterTime);

    assert.commandFailedWithCode(
        db.runCommand({find: "foo", $clusterTime: mismatchingTime}),
        ErrorCodes.TimeProofMismatch,
        "expected the cluster time signature to be validated before the authorization check",
    );

    // Without the mismatching cluster time, the same command gets as far as that check and fails
    // it, which is what makes the assertion above meaningful.
    assert.commandFailedWithCode(
        db.runCommand({find: "foo"}),
        ErrorCodes.Unauthorized,
        "expected the user to be unauthorized for the command",
    );
}

// Start the sharding test with auth on.
const st = new ShardingTest({
    mongos: 1,
    manualAddShard: true,
    other: {keyFile: "jstests/libs/key1"},
});

// Create the cluster-wide admin user, disabling the localhost auth bypass on mongos and the config
// server.
st.s.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
st.s.getDB("admin").auth("foo", "bar");

// Add shard with auth enabled.
const rst = new ReplSetTest({nodes: 2});
rst.startSet({keyFile: "jstests/libs/key1", shardsvr: ""});

rst.initiate(null, "replSetInitiate", {doNotWaitForStableRecoveryTimestamp: true});
const shardPrimary = rst.getPrimary();
assert.commandWorked(st.s.adminCommand({addShard: rst.getURL()}));
const configPrimary = st.configRS.getPrimary();

const testDB = st.s.getDB("test");

// Unsharded collections reject mismatching cluster times and accept valid ones.
assertRejectsMismatchingLogicalTime(testDB);
assertAcceptsValidLogicalTime(testDB);

// Initialize sharding.
assert.commandWorked(testDB.adminCommand({enableSharding: "test"}));
assert.commandWorked(
    testDB.adminCommand({shardCollection: testDB.foo.getFullName(), key: {_id: 1}}),
);

// Sharded collections reject mismatching cluster times and accept valid ones.
assertRejectsMismatchingLogicalTime(testDB);
assertAcceptsValidLogicalTime(testDB);

// A user holding no privileges at all, to exercise a command that fails its authorization check.
testDB.createUser({user: "nobody", pwd: "pwd", roles: []});

// The localhost auth bypass is disabled on mongos and the config server, so unauthenticated clients'
// mismatching signatures are ignored. Log out of mongos first; config starts unauthenticated.
assert.commandWorked(st.s.getDB("admin").logout());
assertIgnoresMismatchingLogicalTimeWhenUnauthenticated(testDB);
assertIgnoresMismatchingLogicalTimeWhenUnauthenticated(configPrimary.getDB("admin"));

// Still on the mongos connection, but now as an authenticated user with no privileges.
assert(testDB.auth("nobody", "pwd"));
assertValidatesBeforeAuthorizationCheck(testDB);
assert.commandWorked(testDB.logout());
assert(st.s.getDB("admin").auth("foo", "bar"));

// The shard has no local users, so a direct connection uses the localhost auth bypass and validates
// the mismatching signature.
assertRejectsMismatchingLogicalTime(shardPrimary.getDB("test"));
assertAcceptsValidLogicalTime(shardPrimary.getDB("test"));

// The config server connection, once authenticated, goes back to rejecting mismatching times.
assert(configPrimary.getDB("admin").auth("foo", "bar"));
assertRejectsMismatchingLogicalTime(configPrimary.getDB("admin"));
assertAcceptsValidLogicalTime(configPrimary.getDB("admin"));

st.stop();
rst.stopSet();
