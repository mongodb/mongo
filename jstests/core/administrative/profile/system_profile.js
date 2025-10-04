// Test various user operations against "system.profile" collection.  SERVER-18111.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: convertToCapped, mapReduce,
//   # profile.
//   not_allowed_with_signed_security_token,
//   does_not_support_stepdowns,
//   requires_capped,
//   requires_collstats,
//   requires_non_retryable_commands,
//   requires_non_retryable_writes,
//   requires_profiling,
//   uses_map_reduce_with_temp_collections,
//   queries_system_profile_collection
// ]

let testDB = db.getSiblingDB("system_profile");
let testDBCopy = db.getSiblingDB("system_profile_copy");

// Create/drop should succeed.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
testDB.system.profile.drop();

// convertToCapped should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
assert.eq(false, testDB.system.profile.stats().capped);
assert.commandFailedWithCode(testDB.system.profile.convertToCapped(1024 * 1024), ErrorCodes.BadValue);
assert.eq(false, testDB.system.profile.stats().capped);

// Basic write operations should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
assert.writeError(testDB.system.profile.insert({}));
assert.writeError(testDB.system.profile.update({}, {a: 1}));
assert.writeError(testDB.system.profile.update({}, {a: 1}, {upsert: true}));
assert.writeError(testDB.system.profile.remove({}));

// Using findAndModify to write to "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
assert.commandFailed(testDB.system.profile.runCommand("findAndModify", {query: {}, update: {a: 1}}));
assert.commandFailed(testDB.system.profile.runCommand("findAndModify", {query: {}, update: {a: 1}, upsert: true}));
assert.commandFailed(testDB.system.profile.runCommand("findAndModify", {query: {}, remove: true}));

// Using mapReduce to write to "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.foo.insert({val: 1}));
assert.commandFailed(
    testDB.foo.runCommand("mapReduce", {
        map: function () {
            emit(0, this.val);
        },
        reduce: function (id, values) {
            return Array.sum(values);
        },
        out: "system.profile",
    }),
);

// Using aggregate to write to "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.foo.insert({val: 1}));
assert.commandFailed(testDB.foo.runCommand("aggregate", {pipeline: [{$out: "system.profile"}]}));

// Renaming to/from "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
assert.commandFailed(
    testDB.adminCommand({renameCollection: testDB.system.profile.getFullName(), to: testDB.foo.getFullName()}),
);
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("foo"));
assert.commandFailed(
    testDB.adminCommand({renameCollection: testDB.foo.getFullName(), to: testDB.system.profile.getFullName()}),
);
