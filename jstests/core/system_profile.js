// Test various user operations against "system.profile" collection.  SERVER-18111.

var testDB = db.getSiblingDB("system_profile");
var testDBCopy = db.getSiblingDB("system_profile_copy");

// Create/drop should succeed.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
testDB.system.profile.drop();

// convertToCapped should succeed.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
assert.eq(false, testDB.system.profile.stats().capped);
assert.commandWorked(testDB.system.profile.convertToCapped(1024 * 1024));
assert.eq(true, testDB.system.profile.stats().capped);

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
assert.commandFailed(
    testDB.system.profile.runCommand("findAndModify", {query: {}, update: {a: 1}}));
assert.commandFailed(
    testDB.system.profile.runCommand("findAndModify", {query: {}, update: {a: 1}, upsert: true}));
assert.commandFailed(testDB.system.profile.runCommand("findAndModify", {query: {}, remove: true}));

// Using mapReduce to write to "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.writeOK(testDB.foo.insert({val: 1}));
assert.commandFailed(testDB.foo.runCommand("mapReduce", {
    map: function() {
        emit(0, this.val);
    },
    reduce: function(id, values) {
        return Array.sum(values);
    },
    out: "system.profile"
}));

// Using aggregate to write to "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.writeOK(testDB.foo.insert({val: 1}));
assert.commandFailed(testDB.foo.runCommand("aggregate", {pipeline: [{$out: "system.profile"}]}));

// Renaming to/from "system.profile" should fail.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("system.profile"));
assert.commandFailed(testDB.adminCommand(
    {renameCollection: testDB.system.profile.getFullName(), to: testDB.foo.getFullName()}));
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("foo"));
assert.commandFailed(testDB.adminCommand(
    {renameCollection: testDB.foo.getFullName(), to: testDB.system.profile.getFullName()}));

// Copying a database containing "system.profile" should succeed.  The "system.profile" collection
// should not be copied.
assert.commandWorked(testDB.dropDatabase());
assert.commandWorked(testDB.createCollection("foo"));
assert.commandWorked(testDB.createCollection("system.profile"));
assert.commandWorked(testDBCopy.dropDatabase());
assert.commandWorked(
    testDB.adminCommand({copydb: 1, fromdb: testDB.getName(), todb: testDBCopy.getName()}));
assert.commandWorked(testDBCopy.foo.stats());
assert.commandFailed(testDBCopy.system.profile.stats());
