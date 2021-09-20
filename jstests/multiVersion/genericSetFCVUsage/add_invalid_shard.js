/**
 * Test that adding invalid or duplicate shards will fail.
 *
 */
(function() {

"use strict";

const st = new ShardingTest({shards: 1});

const configDB = st.s.getDB('config');
const shardDoc = configDB.shards.findOne();

/**
 * @summary Starts a new replica set with the specified binVersion and replicaName.
 * Tests that we can't add that replicaSet as a shard to our existing cluster. Once the assertion
 * completes successfully, it will stop the replica set.
 * @param {string} binVersion The binVersion to configure the replica set with.
 * @param {string} replicaName  The name given to the replica set.
 */
const executeOldBinaryTest = (binVersion) => {
    jsTest.log(`Executing binary test for binVersion ${binVersion}`);
    const oldBinaryReplTest = new ReplSetTest({nodes: 2, nodeOptions: {binVersion, shardsvr: ""}});
    oldBinaryReplTest.startSet();
    oldBinaryReplTest.initiate();

    const connectionString = oldBinaryReplTest.getURL();

    assert.commandFailed(st.admin.runCommand({addshard: connectionString}));
    oldBinaryReplTest.stopSet();
};

/**
 *
 * @callback TestCaseGenerator
 * @param {String} connectionString - The host URL of the replica set.
 * @returns {Array<{addshard: string, name: string, setup: any}>} - An array of test cases to
 *     execute.
 */

/**
 * @summary Receives a function that it can call to generate addshard command objects.
 * It will then go through each command and assert that it will fail. After going through each
 * command, it will stop the replica set.
 * @param {TestCaseGenerator} createStandardTestCases
 */
const executeStandardTests = (createStandardTestCases) => {
    jsTest.log("Starting to execute the standard test cases");
    const replTest = new ReplSetTest({nodes: 2, nodeOptions: {shardsvr: ""}});
    replTest.startSet({oplogSize: 10});
    replTest.initiate();

    const addShardCommands = createStandardTestCases(replTest.getURL());

    addShardCommands.forEach(({addshard, name, setup}) => {
        jsTest.log(`About to run addshard command with value ${addshard} and name ${name}`);
        if (setup) {
            setup();
        }
        assert.commandFailed(st.admin.runCommand({addshard: addshard, name: name}));
    });

    replTest.stopSet();
};

// ---- TEST CASES ----

// Can't add a mongod with a lower binary version than our featureCompatibilityVersion.
executeOldBinaryTest("last-lts");
executeOldBinaryTest("last-continuous");

executeStandardTests((replicaSetConnectionURL) => {
    const truncatedRSConnStr =
        replicaSetConnectionURL.substring(0, replicaSetConnectionURL.indexOf(','));

    return [
        {
            // Can't add replSet as shard if the name doesn't match the replSet config.
            addshard: "prefix_" + replicaSetConnectionURL,
        },
        {
            // Cannot add the same replSet shard host twice when using a unique shard name.
            addshard: replicaSetConnectionURL,
            name: 'dupRS',
            setup: () => (assert.commandWorked(
                st.admin.runCommand({addshard: replicaSetConnectionURL, name: 'dummyRS'})))
        },
        {
            // Cannot add a replica set connection string containing a member that isn't actually
            // part of the replica set.
            addshard: truncatedRSConnStr + 'fakehost',
            name: 'dummyRS'
        },
        {addshard: shardDoc.host, name: 'dupShard'},
        {addshard: st._configDB},  // Can't add config servers as shard.
    ];
});

// Can't add mongos as shard.
assert.commandFailedWithCode(st.admin.runCommand({addshard: st.s.host}),
                             ErrorCodes.IllegalOperation);

st.stop();
})();
