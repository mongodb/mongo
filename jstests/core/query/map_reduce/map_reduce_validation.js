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

const baseName = jsTestName();

const source = db[`${baseName}_source`];
source.drop();
assert.commandWorked(source.insert({x: 1}));

const targetName = `${jsTestName()}_out`;
const viewName = `${jsTestName()}_source_view`;

function mapFunc() {
    emit(this.x, 1);
}
function reduceFunc(key, values) {
    return Array.sum(values);
}

function validateView(viewName) {
    let collectionInfos = db.getCollectionInfos({name: viewName});
    assert.eq(1, collectionInfos.length);
    assert.eq("view", collectionInfos[0].type);
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
    out: {merge: targetName, db: "admin"}
}),
                             ErrorCodes.CommandNotSupported);

assert.commandFailedWithCode(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: targetName, db: "config"}
}),
                             ErrorCodes.CommandNotSupported);

// Test that you can output to a different database.
// Create the other database.
{
    const otherDatabaseName = `${jsTestName()}_other`;
    const otherDB = db.getSiblingDB(otherDatabaseName);
    otherDB[targetName].drop();
    assert.commandWorked(otherDB.createCollection(targetName));
    assert.commandWorked(db.runCommand({
        mapReduce: source.getName(),
        map: mapFunc,
        reduce: reduceFunc,
        out: {merge: targetName, db: otherDatabaseName}
    }));
    assert.eq(otherDB[targetName].find().toArray(), [{_id: 1, value: 1}]);
}

{
    const nonexistentSourceName = `${baseName}_nonexistent`;
    const nonexistentTargetName = `${baseName}_out_nonexistent`;
    db[nonexistentSourceName].drop();
    db[nonexistentTargetName].drop();
    const resultWithNonExistent = db.runCommand({
        mapReduce: nonexistentSourceName,
        map: mapFunc,
        reduce: reduceFunc,
        out: nonexistentTargetName
    });
    if (resultWithNonExistent.ok) {
        // In the implementation which redirects to an aggregation this is expected to succeed and
        // produce an empty output collection.
        assert.commandWorked(resultWithNonExistent);
        assert.eq(db[nonexistentTargetName].find().itcount(), 0);
    } else {
        // In the old MR implementation this is expected to fail.
        assert.commandFailedWithCode(resultWithNonExistent, ErrorCodes.NamespaceNotFound);
    }
}

// Test that you can't use a regex as the namespace.
assert.commandFailed(db.runCommand({
    mapReduce: /mr_validation_bar/,
    map: mapFunc,
    reduce: reduceFunc,
    out: {replace: targetName, db: "test"}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {replace: /mr_validation_foo/, db: "test"}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: /mr_validation_foo/, db: "test"}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {replace: targetName, db: /mr_validation_test/}
}));

assert.commandFailed(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: targetName, db: /mr_validation_test/}
}));

// Test that mapReduce fails when run against a view.
db.getCollection(targetName).drop();
assertDropCollection(db, viewName);
assert.commandWorked(db.createView(viewName, source.getName(), [{$project: {_id: 0}}]));
validateView(viewName);  // sanity check
assert.commandFailedWithCode(
    db.runCommand({mapReduce: viewName, map: mapFunc, reduce: reduceFunc, out: targetName}),
    ErrorCodes.CommandNotSupportedOnView);
validateView(viewName);  // sanity check

// The new implementation is not supported in a sharded cluster yet, so avoid running it in the
// passthrough suites.
if (!FixtureHelpers.isMongos(db)) {
    // Test that mapReduce fails when run against a view.
    db[viewName].drop();
    assert.commandWorked(db.createView(viewName, source.getName(), [{$project: {_id: 0}}]));
    validateView(viewName);  // sanity check
    assert.commandFailedWithCode(
        db.runCommand({mapReduce: viewName, map: mapFunc, reduce: reduceFunc, out: targetName}),
        ErrorCodes.CommandNotSupportedOnView);

    assert.commandFailedWithCode(
        db.runCommand({mapReduce: viewName, map: mapFunc, reduce: reduceFunc, out: targetName}),
        ErrorCodes.CommandNotSupportedOnView);
}
