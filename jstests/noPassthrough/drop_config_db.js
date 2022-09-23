/*
 * Test that dropping the config DB does not crash the server.
 */
(function() {
"use strict";

TestData.enableTestCommands = false;

const mongod = MongoRunner.runMongod();
const config = mongod.getDB('config');

// Create a collection in config to ensure that it exists before dropping it.
assert.commandWorked(config.runCommand({create: 'test'}));
// Dropping the config DB should succeed.
assert.commandWorked(config.dropDatabase());

MongoRunner.stopMongod(mongod);
})();
