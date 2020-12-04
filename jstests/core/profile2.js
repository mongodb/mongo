// Tests that large queries and updates are properly profiled.

// Special db so that it can be run in parallel tests.
// @tags: [requires_profiling]

var coll = db.getSisterDB("profile2").profile2;

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.drop();
coll.getDB().system.profile.drop();
assert.commandWorked(coll.getDB().runCommand({profile: 2}));

/**
 * Asserts that array 'results' contains a profiler generated document that corresponds to a
 * truncated command that matches a regular expression 'truncatedCommandRegexp'. Outputs a message
 * 'message' in case such document is not present.
 */
function assertContainsTruncatedCommand(results, truncatedCommandRegexp, message) {
    const document = results.find(
        element => element.hasOwnProperty('ns') && element.hasOwnProperty('millis') &&
            element.hasOwnProperty('command') && 'string' === typeof(element.command.$truncated) &&
            element.command.$truncated.match(truncatedCommandRegexp));
    assert(document, message + ` Retrieved documents: ${tojson(results)}`);
}

var str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
var hugeStr = str;
while (hugeStr.length < 2 * 1024 * 1024) {
    hugeStr += str;
}

// Test query with large string element.
coll.find({a: hugeStr}).itcount();
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(results,
                               /filter: { a: "a+\.\.\." }//* string value is truncated*/,
                               "Document corresponding to 'find' command not found.");

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.getDB().system.profile.drop();
assert.commandWorked(coll.getDB().runCommand({profile: 2}));

// Test update with large string element in query portion.
assert.writeOK(coll.update({a: hugeStr}, {}));
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /^{ q: { a: "a+\.\.\." }, u: {}, multi: false, upsert: false }$//* string value is truncated*/,
    "Document corresponding to 'update' command not found.");

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.getDB().system.profile.drop();
assert.commandWorked(coll.getDB().runCommand({profile: 2}));

// Test update with large string element in update portion.
assert.writeOK(coll.update({}, {a: hugeStr}));
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /^{ q: {}, u: { a: "a+\.\.\." }, multi: false, upsert: false }$//* string value is truncated*/,
    "Document corresponding to 'update' command not found.");

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.getDB().system.profile.drop();
assert.commandWorked(coll.getDB().runCommand({profile: 2}));

// Test query with many elements in query portion.
var doc = {};
for (var i = 0; i < 100 * 1000; ++i) {
    doc["a" + i] = 1;
}
coll.find(doc).itcount();
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /filter: { a0: 1\.0, a1: .*\.\.\.$//* query object itself is truncated*/,
    "Document corresponding to 'find' command not found.");

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
