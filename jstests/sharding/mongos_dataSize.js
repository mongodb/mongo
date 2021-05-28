/*
 * Tests the dataSize command on mongos.
 */
(function() {
'use strict';

const kDbName = "foo";
const kCollName = "bar";
const kNs = kDbName + "." + kCollName;
const kNumDocs = 100;

/*
 * Returns the global min and max key for the given key pattern.
 */
function getGlobalMinMaxKey(keyPattern) {
    let globalMin = {};
    let globalMax = {};
    for (let field in keyPattern) {
        globalMin[field] = MinKey;
        globalMax[field] = MaxKey;
    }
    return {globalMin, globalMax};
}

/*
 * Runs a dataSize command with the given key pattern on the given connection, and
 * asserts that command works and that the returned numObjects is equal to expectedNumObjects.
 */
function assertDataSizeCmdWorked(conn, keyPattern, expectedNumObjects) {
    let res = assert.commandWorked(conn.adminCommand({dataSize: kNs, keyPattern: keyPattern}));
    assert.eq(res.numObjects, expectedNumObjects);

    const {globalMin, globalMax} = getGlobalMinMaxKey(keyPattern);
    res = assert.commandWorked(
        conn.adminCommand({dataSize: kNs, keyPattern: keyPattern, min: globalMin, max: globalMax}));
    assert.eq(res.numObjects, expectedNumObjects);
}

/*
 * Runs a dataSize command with the given key pattern on the given connection, and
 * asserts that command failed with BadValue error.
 */
function assertDataSizeCmdFailedWithBadValue(conn, keyPattern) {
    assert.commandFailedWithCode(conn.adminCommand({dataSize: kNs, keyPattern: keyPattern}),
                                 ErrorCodes.BadValue);
}

/*
 * Runs dataSize commands on the given connection. Asserts the command fails if run with an invalid
 * namespace or with the min and max as given by each range in invalidRanges. Asserts that the
 * command succeeds if run with valid min and max and returns the expected numObjects.
 */
function testDataSizeCmd(conn, keyPattern, invalidRanges, numObjects) {
    assert.commandFailedWithCode(conn.adminCommand({dataSize: kCollName}),
                                 ErrorCodes.InvalidNamespace);

    for (const {min, max, errorCode} of invalidRanges) {
        const cmdObj = {dataSize: kNs, keyPattern: keyPattern, min: min, max: max};
        assert.commandFailedWithCode(conn.adminCommand(cmdObj), errorCode);
    }

    assertDataSizeCmdWorked(conn, {}, numObjects);
    assertDataSizeCmdWorked(conn, keyPattern, numObjects);
}

const st = new ShardingTest({mongos: 3, shards: 2});
assert.commandWorked(st.s.adminCommand({enableSharding: kDbName}));
st.ensurePrimaryShard(kDbName, st.shard0.shardName);

const shardKey1 = {
    x: 1
};
jsTest.log(`Sharding the collection with key ${tojson(shardKey1)}`);
assert.commandWorked(st.s0.adminCommand({shardCollection: kNs, key: shardKey1}));

let bulk = st.s0.getCollection(kNs).initializeUnorderedBulkOp();
for (let i = 0; i < kNumDocs; ++i) {
    bulk.insert({_id: i, x: i, y: -i});
}
assert.commandWorked(bulk.execute());

jsTest.log("Verify that keyPattern and key range validation works");
const invalidRanges1 = [
    {min: {y: MinKey}, max: {y: MaxKey}, errorCode: ErrorCodes.BadValue},
    {min: {x: MinKey, y: MinKey}, max: {x: MaxKey, y: MaxKey}, errorCode: ErrorCodes.BadValue},
    // The command does not throw any particular error when only one of min or max is specified.
    {min: {}, max: {x: MaxKey}, errorCode: ErrorCodes.UnknownError},
    {min: {x: MinKey}, max: {}, errorCode: ErrorCodes.UnknownError},
];
testDataSizeCmd(st.s0, shardKey1, invalidRanges1, kNumDocs);
testDataSizeCmd(st.s1, shardKey1, invalidRanges1, kNumDocs);
testDataSizeCmd(st.s2, shardKey1, invalidRanges1, kNumDocs);

jsTest.log("Dropping the collection");
st.s0.getCollection(kNs).drop();

const shardKey2 = {
    y: 1
};
jsTest.log(`Resharding the collection with key ${tojson(shardKey2)}`);
assert.commandWorked(st.s0.adminCommand({shardCollection: kNs, key: shardKey2}));

jsTest.log("Verify that validation occurs on shards not on mongos");
// If validation occurs on mongos, the command would fail with BadValue as st.s1 is stale so
// it thinks that shardKey1 is the shard key.
assertDataSizeCmdWorked(st.s1, shardKey2, 0);

// If validation occurs on mongos, the command would succeed as st.s2 is stale so it thinks
// that shardKey1 is the shard key.
assertDataSizeCmdFailedWithBadValue(st.s2, shardKey1);

st.stop();
})();
