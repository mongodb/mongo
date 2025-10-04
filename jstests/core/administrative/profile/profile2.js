// Tests that large queries and updates are properly profiled.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: profile.
//   not_allowed_with_signed_security_token,
//   requires_profiling,
// ]

// Special db so that it can be run in parallel tests.
let coll = db.getSiblingDB("profile2").profile2;

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.drop();
coll.getDB().system.profile.drop();
// Don't profile the setFCV command, which could be run during this test in the
// fcv_upgrade_downgrade_replica_sets_jscore_passthrough suite.
assert.commandWorked(
    coll.getDB().setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

/**
 * Asserts that array 'results' contains a profiler generated document that corresponds to a
 * truncated command that matches a regular expression 'truncatedCommandRegexp'. Outputs a message
 * 'message' in case such document is not present.
 */
function assertContainsTruncatedCommand(results, truncatedCommandRegexp, message) {
    const document = results.find(
        (element) =>
            element.hasOwnProperty("ns") &&
            element.hasOwnProperty("millis") &&
            element.hasOwnProperty("command") &&
            "string" === typeof element.command.$truncated &&
            element.command.$truncated.match(truncatedCommandRegexp),
    );
    assert(document, message + ` Retrieved documents: ${tojson(results)}`);
}

let str = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
let hugeStr = str;
while (hugeStr.length < 2 * 1024 * 1024) {
    hugeStr += str;
}

// Test query with large string element.
coll.find({a: hugeStr}).itcount();
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /filter: { a: "a+\.\.\." }/ /* string value is truncated*/,
    "Document corresponding to 'find' command not found.",
);

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.getDB().system.profile.drop();
assert.commandWorked(
    coll.getDB().setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

// Test update with large string element in query portion.
assert.commandWorked(coll.update({a: hugeStr}, {}));
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /^{ q: { a: "a+\.\.\." }, u: {}, multi: false, upsert: false }$/ /* string value is truncated*/,
    "Document corresponding to 'update' command not found.",
);

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.getDB().system.profile.drop();
assert.commandWorked(
    coll.getDB().setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

// Test update with large string element in update portion.
assert.commandWorked(coll.update({}, {a: hugeStr}));
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /^{ q: {}, u: { a: "a+\.\.\." }, multi: false, upsert: false }$/ /* string value is truncated*/,
    "Document corresponding to 'update' command not found.",
);

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
coll.getDB().system.profile.drop();
assert.commandWorked(
    coll.getDB().setProfilingLevel(1, {filter: {"command.setFeatureCompatibilityVersion": {"$exists": false}}}),
);

// Test query with many elements in query portion.
let doc = {};
for (let i = 0; i < 100 * 1000; ++i) {
    doc["a" + i] = 1;
}
coll.find(doc).itcount();
var results = coll.getDB().system.profile.find().toArray();
assertContainsTruncatedCommand(
    results,
    /filter: { a0: 1\.0, a1: .*\.\.\.$/ /* query object itself is truncated*/,
    "Document corresponding to 'find' command not found.",
);

assert.commandWorked(coll.getDB().runCommand({profile: 0}));
