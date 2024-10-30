// @tags: [
//   # The test runs commands that are not allowed with security token: createUser, dropUser,
//   # logout, profile, setProfilingLevel.
//   not_allowed_with_signed_security_token,
//   assumes_superuser_permissions,
//   creates_and_authenticates_user,
//   does_not_support_stepdowns,
//   requires_capped,
//   requires_collstats,
//   requires_non_retryable_commands,
//   requires_profiling,
//   # This test depends on the strict argument since 6.1.
//   requires_fcv_61,
// ]

const findCommandBatchSize = assert.commandWorked(db.adminCommand(
    {getParameter: 1, internalQueryFindCommandBatchSize: 1}))["internalQueryFindCommandBatchSize"];

// function argument overwritten won't affect original value and it can be run in parallel tests
function profileCursor(testDb, query) {
    query = query || {};
    Object.extend(query, {user: username + "@" + testDb.getName()});
    return testDb.system.profile.find(query);
}

function resetProfile(testDb, level, slowms) {
    testDb.setProfilingLevel(0);
    testDb.system.profile.drop();
    testDb.setProfilingLevel(level, slowms);
}

const testDb = db.getSiblingDB("profile1");
var username = "jstests_profile1_user";

testDb.dropUser(username);
testDb.dropDatabase();

try {
    testDb.createUser({user: username, pwd: "password", roles: jsTest.basicUserRoles});

    testDb.logout();
    testDb.auth(username, "password");

    // expect error given unrecognized options
    assert.commandFailedWithCode(testDb.runCommand({profile: 0, unknown: {}}),
                                 ErrorCodes.IDLUnknownField,
                                 "Expected IDL to reject unknown field for profile command.");

    // With pre-created system.profile (capped)
    testDb.runCommand({profile: 0});
    testDb.getCollection("system.profile").drop();
    assert.eq(0, testDb.runCommand({profile: -1}).was, "A");

    // Create 32MB profile (capped) collection
    testDb.system.profile.drop();
    testDb.createCollection("system.profile", {capped: true, size: 32 * 1024 * 1024});
    testDb.runCommand({profile: 2});
    assert.eq(2, testDb.runCommand({profile: -1}).was, "B");
    assert.eq(1, testDb.system.profile.stats().capped, "C");

    testDb.foo.findOne();

    var profileItems = profileCursor(testDb).toArray();

    // create a msg for later if there is a failure.
    var msg = "";
    profileItems.forEach(function(d) {
        msg += "profile doc: " + d.ns + " " + d.op + " " + tojson(d.query ? d.query : d.command) +
            '\n';
    });
    msg += tojson(testDb.system.profile.stats());

    // If these nunmbers don't match, it is possible the collection has rolled over
    // (set to 32MB above in the hope this doesn't happen).
    // When the 'findCommandBatchSize' is < 3 an additional getMore is required.
    const expectedDocs = findCommandBatchSize < 3 ? 3 : 2;
    assert.eq(expectedDocs, profileItems.length, "E2 -- " + msg);

    // Make sure we can't drop if profiling is still on
    assert.throws(function(z) {
        testDb.getCollection("system.profile").drop();
    });

    // With pre-created system.profile (un-capped)
    testDb.runCommand({profile: 0});
    testDb.getCollection("system.profile").drop();
    assert.eq(0, testDb.runCommand({profile: -1}).was, "F");

    testDb.createCollection("system.profile");
    assert.eq(0, testDb.runCommand({profile: 2}).ok);
    assert.eq(0, testDb.runCommand({profile: -1}).was, "G");
    assert(!testDb.system.profile.stats().capped, "G1");

    // With no system.profile collection
    testDb.runCommand({profile: 0});
    testDb.getCollection("system.profile").drop();
    assert.eq(0, testDb.runCommand({profile: -1}).was, "H");

    testDb.runCommand({profile: 2});
    assert.eq(2, testDb.runCommand({profile: -1}).was, "I");
    assert.eq(1, testDb.system.profile.stats().capped, "J");

    resetProfile(testDb, 2);
    testDb.profile1.drop();
    var q = {_id: 5};
    var u = {$inc: {x: 1}};
    testDb.profile1.update(q, u);
    var r = profileCursor(testDb, {ns: testDb.profile1.getFullName()}).sort({$natural: -1})[0];
    assert.eq({q: q, u: u, multi: false, upsert: false}, r.command, tojson(r));
    assert.eq("update", r.op, tojson(r));
    assert.eq("profile1.profile1", r.ns, tojson(r));
} finally {
    // disable profiling for subsequent tests
    assert.commandWorked(testDb.runCommand({profile: 0}));
    testDb.logout();
}
