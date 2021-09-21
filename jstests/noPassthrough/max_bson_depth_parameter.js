/**
 * Tests that the server properly respects the maxBSONDepth parameter, and will fail to start up if
 * given an invalid depth.
 */
(function() {
"use strict";

const maxBSONDepth = 21;

// Start mongod with a valid BSON depth, then test that it accepts and rejects command
// appropriately based on the depth.
const conn = MongoRunner.runMongod({setParameter: {maxBSONDepth: maxBSONDepth}});
const testDB = conn.getDB("test");

assert.commandWorked(testDB.runCommand({ping: 1}), "Failed to run a command on the server");
assert.commandFailedWithCode(
    testDB.runCommand({
        find: "coll",
        filter: function nestedObj(depth) {
            return {x: depth > 1 ? nestedObj(depth - 1) : 1};
        }(maxBSONDepth + 1),
    }),
    ErrorCodes.Overflow,
    "Expected server to reject command for exceeding the nesting depth limit");

// Confirm depth limits for $lookup.
assert.commandWorked(testDB.coll1.insert({_id: 1}));
assert.commandWorked(testDB.coll2.insert({_id: 1}));

assert.commandWorked(testDB.runCommand({
    aggregate: "coll1",
    pipeline: [{$lookup: {from: "coll2", as: "as", pipeline: []}}],
    cursor: {}
}));
assert.commandFailedWithCode(
    testDB.runCommand(
        {
            aggregate: "coll1",
            pipeline: function nestedPipeline(depth) {
                return [{$lookup: {from: "coll2", as: "as", pipeline: depth > 3 ? nestedPipeline(depth - 3) : []}}];
            }(maxBSONDepth),
            cursor: {}
        }),
    ErrorCodes.Overflow,
    "Expected server to reject command for exceeding the nesting depth limit");

// Restart mongod with a negative maximum BSON depth and test that it fails to start.
MongoRunner.stopMongod(conn);

assert.throws(() => MongoRunner.runMongod({setParameter: "maxBSONDepth=-4"}),
              [],
              "Expected mongod to fail at startup because depth was negative");

assert.throws(() => MongoRunner.runMongod({setParameter: "maxBSONDepth=20"}),
              [],
              "Expected mongod to fail at startup because depth was too low");
}());
