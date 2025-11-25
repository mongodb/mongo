import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const dbName = jsTestName();
const collName = "testColl";
const collName2 = "testColl2";

describe("Test create collection from stale mongos", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1, rs: {nodes: 1}, mongos: 2});

        this.staleMongos = this.st.s0;
        this.nonStaleMongos = this.st.s1;
    });
    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        // Pre-create the collection via the first router.
        assert.commandWorked(this.staleMongos.getDB(dbName).createCollection(collName));

        // Drop the database on another router without removing routing info from first router.
        assert.commandWorked(this.nonStaleMongos.getDB(dbName).dropDatabase());
    });

    it("Create same collection after dropping database via different router", () => {
        assert.commandWorked(this.staleMongos.getDB(dbName).createCollection(collName));

        // Verify that `collName` exists.
        assert.eq(
            1,
            this.nonStaleMongos.getDB(dbName).getCollectionInfos({name: collName}).length,
            "Collection should exist but it doesn't.",
        );
    });

    it("Create different collection after dropping database via different router", () => {
        assert.commandWorked(this.staleMongos.getDB(dbName).createCollection(collName2));

        // Verify that `collName2` exists.
        assert.eq(
            1,
            this.nonStaleMongos.getDB(dbName).getCollectionInfos({name: collName2}).length,
            "Collection should exist but it doesn't.",
        );

        // Verify that `collName` doesn't exist.
        assert.eq(
            0,
            this.nonStaleMongos.getDB(dbName).getCollectionInfos({name: collName}).length,
            "Collection should not exist but it does.",
        );
    });

    it("Insert into collection after dropping database via different router", () => {
        assert.commandWorked(this.staleMongos.getDB(dbName)[collName].insert({_id: 1}));

        // Verify that `collName` exists.
        assert.eq(
            1,
            this.nonStaleMongos.getDB(dbName).getCollectionInfos({name: collName}).length,
            "Collection should exist but it doesn't.",
        );
    });

    it("Shard collection after dropping database via different router", () => {
        assert.commandWorked(this.staleMongos.adminCommand({shardCollection: dbName + "." + collName, key: {x: 1}}));

        // Verify that `collName` exists.
        assert.eq(
            1,
            this.nonStaleMongos.getDB(dbName).getCollectionInfos({name: collName}).length,
            "Collection should exist but it doesn't.",
        );
    });
});
