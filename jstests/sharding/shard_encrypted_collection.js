// Verify valid and invalid scenarios for sharding an encrypted collection

/**
 * @tags: [
 *  requires_fcv_60,
 * ]
 */

// Cannot run the filtering metadata check on tests that run refineCollectionShardKey.
TestData.skipCheckShardFilteringMetadata = true;

(function() {
'use strict';

const st = new ShardingTest({shards: 1, mongos: 1});
const mongos = st.s0;
const kDbName = 'db';

const sampleEncryptedFields = {
    "fields": [
        {
            "path": "firstName",
            "keyId": UUID("11d58b8a-0c6c-4d69-a0bd-70c6d9befae9"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}
        },
        {
            "path": "paymentMethods.creditCards.number",
            "keyId": UUID("12341234-1234-1234-1234-123412341234"),
            "bsonType": "string",
            "queries": {"queryType": "equality"}
        },
    ]
};

// Set up the encrypted collection & enable sharding
assert.commandWorked(
    mongos.getDB(kDbName).createCollection("basic", {encryptedFields: sampleEncryptedFields}));
assert.commandWorked(mongos.adminCommand({enableSharding: kDbName}));

function testShardingCommand(command) {
    jsTestLog("Testing command: " + command);
    let res = null;
    let commandObj = {};
    commandObj[command] = kDbName + '.basic';

    jsTestLog('Fail ' + command + ' if shard key is an encrypted field');
    commandObj['key'] = {firstName: 1};
    res = mongos.adminCommand(commandObj);
    assert.commandFailedWithCode(
        res, ErrorCodes.InvalidOptions, command + " on encrypted field passed");

    commandObj['key'] = {lastName: 1, firstName: "hashed", middleName: 1};
    res = mongos.adminCommand(commandObj);
    assert.commandFailedWithCode(
        res, ErrorCodes.InvalidOptions, command + " on encrypted field passed");

    jsTestLog('Fail ' + command + ' if shard key is a prefix of an encrypted field');
    commandObj['key'] = {"paymentMethods.creditCards": 1};
    res = mongos.adminCommand(commandObj);
    assert.commandFailedWithCode(
        res, ErrorCodes.InvalidOptions, command + " on prefix of encrypted field passed");

    jsTestLog('Fail ' + command + ' if shard key has a prefix matching an encrypted field');
    commandObj['key'] = {"paymentMethods.creditCards.number.lastFour": 1};
    res = mongos.adminCommand(commandObj);
    assert.commandFailedWithCode(
        res, ErrorCodes.InvalidOptions, command + " on key with encrypted field prefix passed");

    jsTestLog('Test ' + command + ' on non-encrypted field works');
    commandObj['key'] = {lastName: 1};
    assert.commandWorked(mongos.adminCommand(commandObj));
}

testShardingCommand("shardCollection");
testShardingCommand("reshardCollection");
testShardingCommand("refineCollectionShardKey");

st.stop();
})();
