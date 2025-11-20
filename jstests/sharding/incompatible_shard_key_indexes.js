/**
 * Tests that an incompatible shard key index can't be the only shard key index on a sharded
 * collection.
 *
 * @tags: [
 *  requires_fcv_80,
 * ]
 */

const shardKeyPattern = {
    skey: 1,
};

const incompatibleShardKeyIndexes = [
    {
        key: {a: 1},
        options: {},
    },
    {
        key: {a: 1, skey: 1},
        options: {},
    },
    {
        key: {skey: 1, multiKeyField: 1},
        options: {},
        isMultiKey: true,
    },
    {
        key: {skey: 1, "a.$**": 1},
        options: {},
    },
    {
        key: {skey: 1},
        options: {hidden: true},
    },
    {
        key: {skey: 1, a: 1},
        options: {hidden: true},
    },
    {
        key: {skey: 1},
        options: {sparse: true},
    },
    {
        key: {skey: 1, a: 1},
        options: {sparse: true},
    },
    {
        key: {skey: 1},
        options: {partialFilterExpression: {dummyField: {$gt: 0}}},
    },
    {
        key: {skey: 1, a: 1},
        options: {partialFilterExpression: {dummyField: {$gt: 0}}},
    },
    {
        key: {skey: 1},
        options: {collation: {locale: "fr_CA"}},
    },
    {
        key: {skey: 1, a: 1},
        options: {collation: {locale: "fr_CA"}},
    },
];

const st = new ShardingTest({shards: 1});
const coll = st.s.getDB("test").coll;

function initTestCase(incompatibleShardKeyIndex) {
    coll.drop();

    // Insert a document that will make the index on 'multiKeyField' multikey.
    coll.insert({skey: 22, multiKeyField: [1, 2, 3]});

    // Create the incompatible index.
    assert.commandWorked(
        coll.createIndex(incompatibleShardKeyIndex.key, incompatibleShardKeyIndex.options),
    );
}

function getIndexByKey(keyPattern) {
    const indexes = coll.getIndexes().filter((index) => friendlyEqual(index.key, keyPattern));
    assert.lte(
        indexes.length,
        1,
        `Found multiple indexes matching key pattern ${tojsononeline(keyPattern)}. Index list: ${
            tojson(indexes)}`,
    );
    assert.eq(
        indexes.length,
        1,
        `Index ${tojsononeline(keyPattern)} not found.`,
    );
    return indexes.pop();
}

for (let incompatibleShardKeyIndex of incompatibleShardKeyIndexes) {
    incompatibleShardKeyIndex.canCoexistWithShardKeyIndex =
        bsonWoCompare(incompatibleShardKeyIndex.key, shardKeyPattern) !== 0;

    jsTestLog("Running tests for " + tojsononeline(incompatibleShardKeyIndex));

    {
        jsTestLog(
            "Verify can't shard a non-empty collection with only an incompatible shard key index");

        initTestCase(incompatibleShardKeyIndex);

        const res = st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKeyPattern});

        coll.insert({skey: 33});

        assert.commandFailedWithCode(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKeyPattern}),
            ErrorCodes.InvalidOptions,
        );
    }

    do {
        jsTestLog(
            "Verify shardCollecion on an empty collection will attempt to create a shard key index");

        initTestCase(incompatibleShardKeyIndex);

        coll.deleteMany({});

        if (incompatibleShardKeyIndex.isMultiKey) {
            // We can't test a multiKey index on an empty collection, since it would not
            // be multiKey.
            break;
        }

        const res = st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKeyPattern});

        assert.commandWorkedOrFailedWithCode(res, [
            ErrorCodes.InvalidOptions,
            ErrorCodes.IndexKeySpecsConflict,
        ]);

        if (res.ok) {
            // The shardCollection command succeeded, check that the shard key index was
            // created and it's not the incompatible index.

            assert(
                incompatibleShardKeyIndex.canCoexistWithShardKeyIndex,
                "reshardCollection succeeded when it should have failed",
            );

            const shardKeyIndex = getIndexByKey(shardKeyPattern);
            const incompatibleIndex = getIndexByKey(incompatibleShardKeyIndex.key);

            assert.neq(shardKeyIndex, null, "shard key index was not created");
            assert.neq(incompatibleIndex, null, "incompatible index is missing");
            assert.neq(incompatibleIndex,
                       shardKeyIndex,
                       "shard key index mustn't be the incompatible index");
        }
    } while (false);

    {
        jsTestLog("Verify can't drop or hide last compatible shard key index");

        initTestCase(incompatibleShardKeyIndex);

        // Shard the collection with a compatible shard key index.
        let compatibleShardKeyIndex = shardKeyPattern;
        if (!incompatibleShardKeyIndex.canCoexistWithShardKeyIndex) {
            compatibleShardKeyIndex = {skey: 1, a: 1};
        }
        assert.commandWorked(coll.createIndex(compatibleShardKeyIndex));
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: compatibleShardKeyIndex}),
        );

        // Attemt to drop or hide the last compatible shard key index.
        assert.commandFailedWithCode(
            coll.dropIndex(compatibleShardKeyIndex),
            ErrorCodes.CannotDropShardKeyIndex,
        );
        assert.commandFailedWithCode(coll.hideIndex(compatibleShardKeyIndex),
                                     ErrorCodes.InvalidOptions);
    }

    {
        jsTestLog("Verify reshardCollection will attempt to create a shard key index");

        initTestCase(incompatibleShardKeyIndex);

        // Shard the collection first with shard key {_id: 1}.
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: {"_id": 1}}),
        );

        // Attempt to reshard the collection to shard key 'shardKeyPattern'.
        const res = st.s.adminCommand({
            reshardCollection: coll.getFullName(),
            key: shardKeyPattern,
            numInitialChunks: 1,
        });

        assert.commandWorkedOrFailedWithCode(res, [
            ErrorCodes.InvalidOptions,
            ErrorCodes.IndexKeySpecsConflict,
        ]);

        if (res.ok) {
            // The reshardCollection command succeeded, check that the shard key index was
            // created and it's not the incompatible index.

            assert(
                incompatibleShardKeyIndex.canCoexistWithShardKeyIndex,
                "reshardCollection succeeded when it should have failed",
            );

            const shardKeyIndex = getIndexByKey(shardKeyPattern);
            const incompatibleIndex = getIndexByKey(incompatibleShardKeyIndex.key);

            assert.neq(shardKeyIndex, null, "shard key index was not created");
            assert.neq(incompatibleIndex, null, "incompatible index is missing");
            assert.neq(incompatibleIndex,
                       shardKeyIndex,
                       "shard key index mustn't be the incompatible index");
        }
    }

    do {
        jsTestLog("can't call refineCollectionShardKey with only an incompatible shard key index");

        initTestCase(incompatibleShardKeyIndex);

        // This test only makes sense if the incompatible shard key index has
        // multiple fields and it's prefixed with the shard key.
        if (!incompatibleShardKeyIndex.canCoexistWithShardKeyIndex ||
            Object.keys(incompatibleShardKeyIndex.key)[0] !== "skey") {
            break;
        }

        // Make sure the collection is sharded on 'shardKeyPattern'.
        coll.createIndex(shardKeyPattern);
        assert.commandWorked(
            st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKeyPattern}),
        );

        // Attempt to refine the shard key to the incompatible shard key index.
        const res = st.s.adminCommand({
            refineCollectionShardKey: coll.getFullName(),
            key: incompatibleShardKeyIndex.key,
        });

        assert.commandFailedWithCode(res, [ErrorCodes.InvalidOptions, ErrorCodes.BadValue]);
    } while (false);
}

st.stop();
