// Tests that invalid options to the mapReduce command are rejected.
// @tags: [
//   uses_map_reduce_with_temp_collections,
//   does_not_support_stepdowns,
// ]
(function() {
"use strict";

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
assert.commandWorked(db.runCommand({
    mapReduce: source.getName(),
    map: mapFunc,
    reduce: reduceFunc,
    out: {merge: "foo", db: "mr_validation_other"}
}));
assert.eq(db.getSiblingDB("mr_validation_other").foo.find().toArray(), [{_id: 1, value: 1}]);

// Test that you can't use a regex as the namespace.
// TODO SERVER-42677 We should be able to expect a single error code here once the parsing logic is
// shared across mongos and mongod. For each of the remaining assertions it looks like there are two
// possible error codes: one for mongos and one for mongod, and each of these are likely different
// than the code we will use once we switch over to the IDL parser, so we'll avoid asserting on the
// error code.
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
}());
