/*
 * Test shardCollection command against a collection without a supporting index for the resharding
 * key does not implicitly create the shard key index if it is run with "implicitlyCreateIndex"
 * false. Also, test that "implicitlyCreateIndex" false is only allowed when the resharding key
 * is hashed.
 *
 * @tags: [
 *   requires_fcv_81,
 *   featureFlagHashedShardKeyIndexOptionalUponShardingCollection
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

function runTest(hashedShardKeyIndexOptionalUponShardingCollection) {
    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            setParameter: {
                featureFlagHashedShardKeyIndexOptionalUponShardingCollection:
                    hashedShardKeyIndexOptionalUponShardingCollection,
            }
        }
    });

    for (let implicitlyCreateIndex of [true, false]) {
        jsTest.log("Testing reshardCollection " + tojson({
                       implicitlyCreateIndex,
                       hashedShardKeyIndexOptionalUponShardingCollection,
                   }));
        const dbName = "testDb";
        const collName = "testColl";
        const ns = dbName + "." + collName;
        const coll = st.s.getCollection(ns);
        let expectedShardKey;
        let expectedExistingIndexKeys = [];
        let expectedNonExistingIndexKeys = [];

        let validateCollection = () => {
            const configCollDoc = st.s.getCollection("config.collections").findOne({_id: ns});
            assert.neq(configCollDoc, null);
            assert.eq(bsonWoCompare(configCollDoc.key, expectedShardKey), 0, configCollDoc);

            const actualIndexSpecs = st.s.getDB(dbName).getCollection(collName).getIndexes();
            expectedExistingIndexKeys.forEach(indexKey => {
                assert.eq(actualIndexSpecs.some(
                              actualIndexSpec => bsonWoCompare(actualIndexSpec.key, indexKey) == 0),
                          true,
                          {actualIndexSpecs, indexKey});
            });
            expectedNonExistingIndexKeys.forEach(indexKey => {
                assert.eq(actualIndexSpecs.some(
                              actualIndexSpec => bsonWoCompare(actualIndexSpec.key, indexKey) == 0),
                          false,
                          {actualIndexSpecs, indexKey});
            });
        };

        const shardKey0 = {a: 1};
        assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: shardKey0}));
        expectedShardKey = shardKey0;
        expectedExistingIndexKeys.push(shardKey0);

        const otherIndexKey = {b: 1};
        assert.commandWorked(coll.createIndex(otherIndexKey));
        expectedExistingIndexKeys.push(otherIndexKey);

        jsTest.log("Testing reshardCollection with hashed shard key");
        const shardKey1 = {c: "hashed"};
        const res1 =
            st.s.adminCommand({reshardCollection: ns, key: shardKey1, implicitlyCreateIndex});
        if (implicitlyCreateIndex || hashedShardKeyIndexOptionalUponShardingCollection) {
            assert.commandWorked(res1);
            expectedShardKey = shardKey1;
            if (implicitlyCreateIndex) {
                expectedExistingIndexKeys.push(shardKey1);
            } else {
                expectedNonExistingIndexKeys.push(shardKey1);
            }
        } else {
            assert.commandFailedWithCode(res1, ErrorCodes.InvalidOptions);
            expectedNonExistingIndexKeys.push(shardKey1);
        }
        validateCollection();

        jsTest.log("Testing reshardCollection with range shard key");
        const shardKey2 = {d: 1};
        const res2 =
            st.s.adminCommand({reshardCollection: ns, key: shardKey2, implicitlyCreateIndex});
        if (implicitlyCreateIndex) {
            assert.commandWorked(res2);
            expectedShardKey = shardKey2;
            expectedExistingIndexKeys.push(shardKey2);
        } else {
            assert.commandFailedWithCode(res2, ErrorCodes.InvalidOptions);
            expectedNonExistingIndexKeys.push(shardKey2);
        }
        validateCollection();

        assert(coll.drop());
    }

    st.stop();
}

for (let hashedShardKeyIndexOptionalUponShardingCollection of [true, false]) {
    runTest(hashedShardKeyIndexOptionalUponShardingCollection);
}
