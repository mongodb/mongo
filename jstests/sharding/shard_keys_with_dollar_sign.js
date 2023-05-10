/**
 * Tests that the shardCollection command correctly rejects a shard key that has a field name that
 * starts with '$' or contains parts that start with '$' unless the part is a DBRef (i.e. is equal
 * to '$id', '$db' or '$ref').
 *
 * @tags: [requires_fcv_44]
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 1});

const dbName = "testDb";
const ns0 = dbName + ".testColl0";
const ns1 = dbName + ".testColl1";
const db = st.s.getDB(dbName);

function testValidation(key, {isValidIndexKey, isValidShardKey}) {
    jsTest.log(`Testing ${tojson({key, isValidIndexKey, isValidShardKey})}`);
    assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
    st.ensurePrimaryShard(dbName, st.shard0.name);

    const createIndexRes = db.getCollection(ns0).createIndex(key);
    if (isValidIndexKey) {
        assert.commandWorked(createIndexRes);
    } else {
        assert.commandFailedWithCode(createIndexRes, ErrorCodes.CannotCreateIndex);
    }

    const shardCollectionRes = st.s.adminCommand({shardCollection: ns1, key});
    if (isValidShardKey) {
        assert.commandWorked(shardCollectionRes);
    } else {
        assert.commandFailedWithCode(shardCollectionRes, ErrorCodes.BadValue);
    }

    assert.commandWorked(db.dropDatabase());
}

testValidation({"$x": 1}, {isValidIndexKey: false, isValidShardKey: false});
testValidation({"x.$y": 1}, {isValidIndexKey: false, isValidShardKey: false});
testValidation({"$**": 1}, {isValidIndexKey: true, isValidShardKey: false});
testValidation({"x.$**": 1}, {isValidIndexKey: true, isValidShardKey: false});
testValidation({"$": 1}, {isValidIndexKey: false, isValidShardKey: false});

testValidation({"x$": 1}, {isValidIndexKey: true, isValidShardKey: true});
testValidation({"x$.y": 1}, {isValidIndexKey: true, isValidShardKey: true});
testValidation({"x.y$": 1}, {isValidIndexKey: true, isValidShardKey: true});

// Verify that a shard key can have a field that contains a DBRef as long as the field itself
// does not start with '$'.
testValidation({"$id": 1}, {isValidIndexKey: false, isValidShardKey: false});
testValidation({"$db": 1}, {isValidIndexKey: false, isValidShardKey: false});
testValidation({"$ref": 1}, {isValidIndexKey: false, isValidShardKey: false});
testValidation({"x.$id": 1}, {isValidIndexKey: true, isValidShardKey: true});
testValidation({"x.$db": 1}, {isValidIndexKey: true, isValidShardKey: true});
testValidation({"x.$ref": 1}, {isValidIndexKey: true, isValidShardKey: true});

st.stop();
})();
