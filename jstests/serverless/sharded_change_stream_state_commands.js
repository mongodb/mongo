// Test that the 'setChangeStreamState' and 'getChangeStreamState' commands work as expected in the
// multi-tenant sharded cluster environment for various cases.
// @tags: [
//   requires_fcv_63,
// ]
(function() {
"use strict";

const shardingTest = new ShardingTest({
    shards: 2,
    other: {configOptions: {setParameter: {featureFlagServerlessChangeStreams: true}}}
});

// TODO SERVER-68341 Implement tests for mongoQ and ensure that the change collection is not enabled
// on the config server.
const configPrimary = shardingTest.configRS.getPrimary();
assert.commandFailedWithCode(
    configPrimary.getDB("admin").runCommand({setChangeStreamState: 1, enabled: true}),
    ErrorCodes.CommandNotSupported);
assert.commandFailedWithCode(configPrimary.getDB("admin").runCommand({getChangeStreamState: 1}),
                             ErrorCodes.CommandNotSupported);

shardingTest.stop();
}());
