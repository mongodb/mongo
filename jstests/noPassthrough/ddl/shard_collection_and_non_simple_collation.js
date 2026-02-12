/**
 * Tests the interaction between shard keys and indexes with simple vs non-simple collation.
 *
 * @tags: [
 *  multiversion_incompatible,
 *  requires_sharding,
 * ]
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("shard collection and non-simple collation tests", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1});
        this.db = this.st.s.getDB("test");
        this.collName = "coll";
        this.collName2 = "coll2";
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        this.db[this.collName].drop();
    });

    it("creates a collection implicitly when creating a unique index with simple collation and shards the collection successfully", () => {
        const coll = this.db[this.collName];
        const coll2 = this.db[this.collName2];
        const shardKey = {a: 1};

        // Create unique index with simple collation (default).
        assert.commandWorked(coll.createIndex(shardKey, {unique: true, collation: {locale: "simple"}}));

        // Shard the collection - should succeed because the index has simple collation.
        assert.commandWorked(this.st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

        // Shard another collection with explicit simple collation in the shardCollection command.
        assert.commandWorked(
            this.st.s.adminCommand({
                shardCollection: coll2.getFullName(),
                key: shardKey,
                unique: true,
                collation: {locale: "simple"},
            }),
        );
    });

    it("creates a collection implicitly when creating a unique index with non-simple collation and fails to shard the collection", () => {
        const coll = this.db[this.collName];
        const shardKey = {a: 1};

        // Create unique index with non-simple collation.
        assert.commandWorked(
            coll.createIndex(shardKey, {unique: true, collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}),
        );

        // Attempt to shard the collection - should fail because the index has non-simple collation.
        assert.commandFailedWithCode(
            this.st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}),
            ErrorCodes.InvalidOptions,
        );
    });

    it("shards a collection and successfully creates a non-simple collation index with the same key format", () => {
        const coll = this.db[this.collName];
        const shardKey = {a: 1};

        // Shard the collection first.
        assert.commandWorked(this.st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

        // Create a non-unique index with non-simple collation - should succeed.
        assert.commandWorked(coll.createIndex(shardKey, {collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}));
    });

    it("shards a collection and fails to create a unique non-simple collation index with the same key format", () => {
        const coll = this.db[this.collName];
        const shardKey = {a: 1};

        // Shard the collection first.
        assert.commandWorked(this.st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

        // Attempt to create a unique index with non-simple collation - should fail.
        assert.commandFailedWithCode(
            coll.createIndex(shardKey, {unique: true, collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}),
            ErrorCodes.CannotCreateIndex,
        );
    });

    it("shards a collection and fails to use collMod to change prepareUnique with non-simple collation", () => {
        const coll = this.db[this.collName];
        const shardKey = {a: 1};

        // Shard the collection.
        assert.commandWorked(this.st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));

        assert.commandWorked(coll.createIndex(shardKey, {collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"}));

        // Attempt to use collMod to change prepareUnique and collation to non-simple - should fail.
        assert.commandFailedWithCode(
            this.db.runCommand({
                collMod: this.collName,
                index: {keyPattern: shardKey, name: "a_1_enUS", prepareUnique: true},
            }),
            ErrorCodes.InvalidOptions,
        );
    });

    it("shards a collection and fails to create a unique non-simple collation index with _id prefix", () => {
        const coll = this.db[this.collName];
        const shardKey = {a: 1};

        // Shard the collection
        assert.commandWorked(this.st.s.adminCommand({shardCollection: coll.getFullName(), key: shardKey}));
        // Attempt to create a unique index with non-simple collation - should fail.
        assert.commandFailedWithCode(
            coll.createIndex(
                {_id: 1, b: 1},
                {unique: true, collation: {locale: "en_US", strength: 2}, name: "a_1_enUS"},
            ),
            ErrorCodes.CannotCreateIndex,
        );

        // Attempt to create a unique index with simple collation - should succeed.
        assert.commandWorked(
            coll.createIndex(
                {_id: 1, b: 1},
                {unique: true, collation: {locale: "simple", strength: 2}, name: "a_1_simple"},
            ),
        );
    });
});
