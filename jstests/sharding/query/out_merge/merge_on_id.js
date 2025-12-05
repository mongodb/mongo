// Tests that the _id index can only support $merge on unsharded collections or when it is at the start of a shard key.
// @tags: [
//   requires_fcv_83
// ]

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("$merge on _id should only work when _id is guaranteed to be unique across all shards", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2, other: {enableBalancer: true}});

        this.db = this.st.s0.getDB("merge_on_id");
        this.sourceColl = this.db.source;
        this.sourceColl.drop();
        assert.commandWorked(this.sourceColl.insert({_id: 1, a: 2, b: 3}));
    });

    after(() => {
        this.st.stop();
    });

    const kExpectedErrorCodes = [51190, 51183, 1074330];

    function getMergePipeline(coll) {
        return [
            {
                $merge: {
                    into: coll.getName(),
                    on: "_id",
                    whenMatched: "replace",
                    whenNotMatched: "insert",
                },
            },
        ];
    }

    it("should allow $merge on unsharded collection", () => {
        const destinationColl = this.db.destination;
        destinationColl.drop();
        for (let i = 0; i < 10; i++) {
            assert.commandWorked(destinationColl.insert({_id: i, a: i + 1, b: i + 2}));
        }

        assert.eq(0, this.sourceColl.aggregate(getMergePipeline(destinationColl)).itcount());
    });

    it("should allow $merge on sharded collection when _id is the shard key", () => {
        const destinationColl = this.db.destination_sharded_id;
        destinationColl.drop();

        this.st.shardColl(destinationColl.getName(), {_id: 1}, {_id: 0}, {_id: 0}, this.db.getName());

        for (let i = 0; i < 10; i++) {
            assert.commandWorked(destinationColl.insert({_id: i, a: i + 1, b: i + 2}));
        }

        assert.eq(0, this.sourceColl.aggregate(getMergePipeline(destinationColl)).itcount());
    });

    it("should not allow $merge on sharded collection when _id is a prefix of the shard key", () => {
        const destinationColl = this.db.destination_sharded_id_prefix;
        destinationColl.drop();

        this.st.shardColl(destinationColl.getName(), {_id: 1, a: 1}, {_id: 0, a: 0}, {_id: 0, a: 0}, this.db.getName());

        assert.commandWorked(destinationColl.insertOne({_id: 0, a: 1}));
        assert.commandWorked(destinationColl.insertOne({_id: 0, a: -1}));

        assert.throwsWithCode(() => {
            this.sourceColl.aggregate(getMergePipeline(destinationColl)).itcount();
        }, kExpectedErrorCodes);
    });

    it("should not allow $merge on sharded collection when _id is not part of the shard key", () => {
        const destinationColl = this.db.destination_sharded_no_id;
        destinationColl.drop();

        this.st.shardColl(destinationColl.getName(), {a: 1}, {a: 0}, {a: 0}, this.db.getName());

        assert.commandWorked(destinationColl.insertOne({_id: 0, a: 1}));
        assert.commandWorked(destinationColl.insertOne({_id: 0, a: -1}));

        assert.throwsWithCode(() => {
            this.sourceColl.aggregate(getMergePipeline(destinationColl)).itcount();
        }, kExpectedErrorCodes);
    });

    it("should not allow $merge on sharded collection when _id is not the prefix of the shard key", () => {
        const destinationColl = this.db.destination_sharded_id_not_prefix;
        destinationColl.drop();

        this.st.shardColl(destinationColl.getName(), {a: 1, _id: 1}, {a: 0, _id: 0}, {a: 0, _id: 0}, this.db.getName());

        assert.commandWorked(destinationColl.insertOne({_id: 0, a: 1}));
        assert.commandWorked(destinationColl.insertOne({_id: 0, a: -1}));

        assert.throwsWithCode(() => {
            this.sourceColl.aggregate(getMergePipeline(destinationColl)).itcount();
        }, kExpectedErrorCodes);
    });
});
