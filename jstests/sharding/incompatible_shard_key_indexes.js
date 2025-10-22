/**
 * Tests that an incompatible shard key index can't be the only shard key index on a sharded
 * collection.
 *
 * @tags: [
 *  requires_fcv_83,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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
        makeItMultiKey: function (coll) {
            coll.insert({skey: 10, multiKeyField: [1, 2, 3]});
        },
    },
    {
        key: {skey: 1},
        options: {},
        makeItMultiKey: function (coll) {
            coll.insert({skey: [1, 2, 3]});
        },
        skeyFieldIsMultikey: true,
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

describe("testing incompatible shard key indexes", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1});
        this.coll = this.st.s.getDB("test").coll;
    });

    after(() => {
        this.st.stop();
    });

    for (let incompatibleShardKeyIndex of incompatibleShardKeyIndexes) {
        incompatibleShardKeyIndex.canCoexistWithShardKeyIndex =
            bsonWoCompare(incompatibleShardKeyIndex.key, shardKeyPattern) !== 0;

        describe("test specific incompatible shard key index", () => {
            before(() => {
                jsTest.log.info("Running tests for", incompatibleShardKeyIndex);
            });

            beforeEach(() => {
                this.coll.drop();

                // Make the index multikey if necessary.
                if (incompatibleShardKeyIndex.makeItMultiKey) {
                    incompatibleShardKeyIndex.makeItMultiKey(this.coll);
                }

                // Create the incompatible index.
                assert.commandWorked(
                    this.coll.createIndex(incompatibleShardKeyIndex.key, incompatibleShardKeyIndex.options),
                );
            });

            it("can't shard a non-empty collection with only an incompatible shard key index", () => {
                this.coll.insert({skey: 33});

                assert.commandFailedWithCode(
                    this.st.s.adminCommand({shardCollection: this.coll.getFullName(), key: shardKeyPattern}),
                    [ErrorCodes.InvalidOptions],
                );
            });

            it("shardCollecion on an empty collection will attempt to create a shard key index", () => {
                this.coll.deleteMany({});

                if (incompatibleShardKeyIndex.makeItMultiKey) {
                    this.coll.remove({});
                }

                const res = this.st.s.adminCommand({shardCollection: this.coll.getFullName(), key: shardKeyPattern});

                if (!incompatibleShardKeyIndex.canCoexistWithShardKeyIndex) {
                    assert.commandFailedWithCode(res, [
                        ErrorCodes.IndexOptionsConflict,
                        ErrorCodes.IndexKeySpecsConflict,
                        ErrorCodes.InvalidOptions,
                    ]);
                    return;
                }

                assert.commandWorked(res);

                // A valid shard key index should be implicitly created during the reshardCollection
                // operation.
                const shardKeyIndex = this.coll.getIndexByKey(shardKeyPattern);
                const incompatibleIndex = this.coll.getIndexByKey(incompatibleShardKeyIndex.key);

                assert.neq(shardKeyIndex, null, "shard key index was not created");
                assert.neq(incompatibleIndex, null, "incompatible index is missing");
                assert.neq(incompatibleIndex, shardKeyIndex, "shard key index mustn't be the incompatible index");
            });

            it("can't drop or hide last compatible shard key index", () => {
                if (incompatibleShardKeyIndex.skeyFieldIsMultikey) {
                    // Can't create a compatible shard key index if the shard key field is multikey.
                    return;
                }

                // Shard the collection with a compatible shard key index.
                let compatibleShardKeyIndex = shardKeyPattern;
                if (!incompatibleShardKeyIndex.canCoexistWithShardKeyIndex) {
                    compatibleShardKeyIndex = {skey: 1, a: 1};
                }
                assert.commandWorked(this.coll.createIndex(compatibleShardKeyIndex));
                assert.commandWorked(
                    this.st.s.adminCommand({shardCollection: this.coll.getFullName(), key: compatibleShardKeyIndex}),
                );

                // Attemt to drop or hide the last compatible shard key index.
                assert.commandFailedWithCode(
                    this.coll.dropIndex(compatibleShardKeyIndex),
                    ErrorCodes.CannotDropShardKeyIndex,
                );
                assert.commandFailedWithCode(this.coll.hideIndex(compatibleShardKeyIndex), ErrorCodes.InvalidOptions);
            });

            it("reshardCollection will attempt to create a shard key index", () => {
                // Shard the collection first with shard key {_id: 1}.
                assert.commandWorked(
                    this.st.s.adminCommand({shardCollection: this.coll.getFullName(), key: {"_id": 1}}),
                );

                // Attempt to reshard the collection to shard key 'shardKeyPattern'.
                const res = this.st.s.adminCommand({
                    reshardCollection: this.coll.getFullName(),
                    key: shardKeyPattern,
                    numInitialChunks: 1,
                });

                if (!incompatibleShardKeyIndex.canCoexistWithShardKeyIndex) {
                    assert.commandFailedWithCode(res, [
                        ErrorCodes.IndexOptionsConflict,
                        ErrorCodes.IndexKeySpecsConflict,
                        ErrorCodes.InvalidOptions,
                    ]);
                    return;
                }

                assert.commandWorked(res);

                // A valid shard key index should be implicitly created during the reshardCollection
                // operation.
                assert(
                    incompatibleShardKeyIndex.canCoexistWithShardKeyIndex,
                    "reshardCollection succeeded when it should have failed",
                );

                const shardKeyIndex = this.coll.getIndexByKey(shardKeyPattern);
                const incompatibleIndex = this.coll.getIndexByKey(incompatibleShardKeyIndex.key);

                assert.neq(shardKeyIndex, null, "shard key index was not created");
                assert.neq(incompatibleIndex, null, "incompatible index is missing");
                assert.neq(incompatibleIndex, shardKeyIndex, "shard key index mustn't be the incompatible index");
            });

            it("can't call refineCollectionShardKey with only an incompatible shard key index", () => {
                // This test only makes sense if the incompatible shard key index has
                // multiple fields and it's prefixed with the shard key.
                if (
                    !incompatibleShardKeyIndex.canCoexistWithShardKeyIndex ||
                    Object.keys(incompatibleShardKeyIndex.key)[0] !== "skey"
                ) {
                    return;
                }

                // Make sure the collection is sharded on 'shardKeyPattern'.
                this.coll.createIndex(shardKeyPattern);
                assert.commandWorked(
                    this.st.s.adminCommand({shardCollection: this.coll.getFullName(), key: shardKeyPattern}),
                );

                // Attempt to refine the shard key to the incompatible shard key index.
                const res = this.st.s.adminCommand({
                    refineCollectionShardKey: this.coll.getFullName(),
                    key: incompatibleShardKeyIndex.key,
                });

                assert.commandFailedWithCode(res, [ErrorCodes.InvalidOptions, ErrorCodes.BadValue]);
            });
        });
    }
});
