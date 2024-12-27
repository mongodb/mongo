/**
 * Tests the express code path, which bypasses regular query planning and execution, for writes.
 * Very similar to express_write.js, but for test cases which assumes no implicit collection
 * creation after drop.
 *
 * @tags: [
 *   requires_fcv_80,
 *   requires_non_retryable_commands,
 *   requires_non_retryable_writes,
 *   assumes_no_implicit_collection_creation_after_drop,
 *   requires_getmore,
 * ]
 */

import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {isExpress} from "jstests/libs/query/analyze_plan.js";

const collName = 'express_write_explicit_coll_creation';
const coll = db.getCollection(collName);
const docs = [
    {_id: 0, a: 0},
    {_id: "str", a: 1},
];

function runExpressTest({command, expectedDocs, usesExpress}) {
    // Run the command to make sure it succeeds.
    assert.commandWorked(db.runCommand(command));
    assertArrayEq({
        actual: coll.find().toArray(),
        expected: expectedDocs,
        extraErrorMsg: "Result set comparison failed for command: " + tojson(command)
    });

    // Reset the collection docs then run explain.
    assert.commandWorked(coll.remove({}));
    assert.commandWorked(coll.insert(docs));
    const explain =
        assert.commandWorked(db.runCommand({explain: command, verbosity: "executionStats"}));

    assert.eq(
        usesExpress,
        isExpress(db, explain),
        "Expected the query to " + (usesExpress ? "" : "not ") + "use express: " + tojson(explain));
}

//
// Tests the behavior of the express path with collation. The express path is used when the query
// and collection collations match, or when the query collation is not specified.
//
const caseInsensitive = {
    locale: "en_US",
    strength: 2
};
const french = {
    locale: "fr"
};

coll.drop();
assert.commandWorked(db.createCollection(collName, {collation: caseInsensitive}));
assert.commandWorked(coll.insert(docs));

// Delete.
const del = {
    q: {_id: "StR"},
    limit: 0
};
runExpressTest(
    {command: {delete: collName, deletes: [del]}, usesExpress: true, expectedDocs: [docs[0]]});
runExpressTest({
    command: {delete: collName, deletes: [{...del, collation: caseInsensitive}]},
    usesExpress: true,
    expectedDocs: [docs[0]]
});
runExpressTest({
    command: {delete: collName, deletes: [{...del, collation: french}]},
    usesExpress: false,
    expectedDocs: docs
});

// findAndModify.
const findAndModifyCommand = {
    findAndModify: collName,
    query: {_id: "StR"},
    update: [{$set: {c: 1}}]
};
runExpressTest({
    command: findAndModifyCommand,
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}]
});
runExpressTest({
    command: {...findAndModifyCommand, collation: caseInsensitive},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}]
});
runExpressTest({
    command: {...findAndModifyCommand, collation: french},
    usesExpress: false,
    expectedDocs: docs
});

// Update.
const update = {
    q: {_id: "StR"},
    u: {$set: {c: 1}}
};
runExpressTest({
    command: {update: collName, updates: [update]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}]
});
runExpressTest({
    command: {update: collName, updates: [{...update, collation: caseInsensitive}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}]
});
runExpressTest({
    command: {update: collName, updates: [{...update, collation: french}]},
    usesExpress: false,
    expectedDocs: docs
});

//
// The express path can be used for writes against clustered collections.
//
coll.drop();
assert.commandWorked(
    db.createCollection(collName, {clusteredIndex: {key: {'_id': 1}, unique: true}}));
assert.commandWorked(coll.insert(docs));

runExpressTest({
    command: {delete: collName, deletes: [{q: {_id: 0}, limit: 0}]},
    usesExpress: true,
    expectedDocs: [docs[1]]
});
runExpressTest({
    command: {findAndModify: collName, query: {_id: "str"}, update: [{$set: {c: 1}}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}]
});
runExpressTest({
    command: {update: collName, updates: [{q: {_id: "str"}, u: {$set: {c: 1}}}]},
    usesExpress: true,
    expectedDocs: [docs[0], {_id: "str", a: 1, c: 1}]
});
