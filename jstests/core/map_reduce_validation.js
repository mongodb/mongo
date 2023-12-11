// Tests that invalid options to the mapReduce command are rejected.
//
// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   assumes_no_implicit_collection_creation_after_drop,
//   does_not_support_stepdowns,
//   uses_map_reduce_with_temp_collections,
// ]
import {assertDropCollection} from "jstests/libs/collection_drop_recreate.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";

const source = db.mr_validation;
source.drop();
assert.commandWorked(source.insert({x: 1}));

function mapFunc() {
    emit(this.x, 1);
}
function reduceFunc(key, values) {
    return Array.sum(values);
}

// Test that you can't specify sharded and inline.
assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {inline: 1, sharded: true}
}));

// Test that you can't output to the admin or config databases.
assert.commandFailedWithCode(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: "foo", db: "admin"}
}),
                             ErrorCodes.CommandNotSupported);

assert.commandFailedWithCode(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: "foo", db: "config"}
}),
                             ErrorCodes.CommandNotSupported);

// Test that you can output to a different database.
// Create the other database.
db.getSiblingDB("mr_validation_other").foo.drop();
assert.commandWorked(db.getSiblingDB("mr_validation_other").createCollection("foo"));
assert.commandWorked(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: "foo", db: "mr_validation_other"}
}));
assert.eq(db.getSiblingDB("mr_validation_other").foo.find().toArray(), [{_id: 1, value: 1}]);

db.mr_nonexistent.drop();
db.out_nonexistent.drop();
const resultWithNonExistent = db.runCommand(
    {mapReduce: "mr_nonexistent", map: mapFunc, reduce: reduceFunc, out: "out_nonexistent"});
if (resultWithNonExistent.ok) {
    // In the implementation which redirects to an aggregation this is expected to succeed and
    // produce an empty output collection.
    assert.commandWorked(resultWithNonExistent);
    assert.eq(db.out_nonexistent.find().itcount(), 0);
} else {
    // In the old MR implementation this is expected to fail.
    assert.commandFailedWithCode(resultWithNonExistent, ErrorCodes.NamespaceNotFound);
}

// Test that you can't use a regex as the namespace.
assert.commandFailed(db.runCommand(
    {mapReduce: /bar/, map: mapFunc, reduce: reduceFunc, out: {replace: "foo", db: "test"}}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {replace: /foo/, db: "test"}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: /foo/, db: "test"}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {replace: "foo", db: /test/}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: "foo", db: /test/}
}));

// Test that mapReduce fails when run against a view.
assertDropCollection(db, "sourceView");
assert.commandWorked(db.createView("sourceView", source.getName(), [{$project: {_id: 0}}]));
assert.commandFailedWithCode(
    db.runCommand({mapReduce: "sourceView", map: mapFunc, reduce: reduceFunc, out: "foo"}),
    ErrorCodes.CommandNotSupportedOnView);

// The new implementation is not supported in a sharded cluster yet, so avoid running it in the
// passthrough suites.
if (!FixtureHelpers.isMongos(db)) {
    // Test that mapReduce fails when run against a view.
    db.sourceView.drop();
    assert.commandWorked(db.createView("sourceView", source.getName(), [{$project: {_id: 0}}]));
    assert.commandFailedWithCode(
        db.runCommand({mapReduce: "sourceView", map: mapFunc, reduce: reduceFunc, out: "foo"}),
        ErrorCodes.CommandNotSupportedOnView);

    assert.commandFailedWithCode(
        db.runCommand({mapReduce: "sourceView", map: mapFunc, reduce: reduceFunc, out: "foo"}),
        ErrorCodes.CommandNotSupportedOnView);
}
