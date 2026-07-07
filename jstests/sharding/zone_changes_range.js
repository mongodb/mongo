/**
 * Test that chunks and documents are moved correctly after zone changes.
 *
 * Each scenario runs on a fresh sharded collection so that chunk ownership recorded by earlier
 * scenarios never carries over. Chunk layouts are built with whole-chunk moves, and the sub-ranges
 * moved in the key-range-change scenarios are directed to shards that never owned the wider range,
 * so a migration is never rejected for overlapping a still-reachable former owner.
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {
    assertChunksOnShards,
    assertDocsOnShards,
    assertShardTags,
    moveZoneToShard,
    runBalancer,
    updateZoneKeyRange,
} from "jstests/sharding/libs/zone_changes_util.js";

describe("zone changes move chunks and documents correctly", function () {
    before(() => {
        this.st = new ShardingTest({shards: 3, other: {chunkSize: 1}});
        this.shard0 = this.st.shard0;
        this.shard1 = this.st.shard1;
        this.shard2 = this.st.shard2;
        this.configDB = this.st.s.getDB("config");
        this.shardKey = {x: 1};
        const bigString = "X".repeat(1024 * 1024); // 1MB
        this.docs = [{x: -15}, {x: -5}, {x: 5}, {x: 15}, {x: 25}].map((d) => ({
            ...d,
            s: bigString,
        }));
        this.dbCounter = 0;
    });

    after(() => {
        this.st.stop();
    });

    // Removes every zone key range and every shard-zone association left by the previous scenario.
    // Ranges must be cleared before the shard-zone tags, otherwise removeShardFromZone reports the
    // zone as still in use.
    const resetZones = () => {
        for (const tag of this.configDB.tags.find().toArray()) {
            assert.commandWorked(
                this.st.s.adminCommand({
                    updateZoneKeyRange: tag.ns,
                    min: tag.min,
                    max: tag.max,
                    zone: null,
                }),
            );
        }
        for (const shard of this.configDB.shards.find().toArray()) {
            for (const zone of shard.tags || []) {
                assert.commandWorked(
                    this.st.s.adminCommand({removeShardFromZone: shard._id, zone}),
                );
            }
        }
    };

    beforeEach(() => {
        this.st.stopBalancer();
        resetZones();
        // Fresh sharded namespace per scenario so no chunk ownership leaks between cases.
        this.dbName = `${jsTestName()}_${this.dbCounter++}`;
        this.ns = `${this.dbName}.range`;
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.shard0.shardName,
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: this.ns, key: this.shardKey}),
        );
    });

    // Splits the collection at each given shard-key value.
    const splitAt = (points) => {
        for (const p of points) {
            assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: p}}));
        }
    };

    const insertDocs = () => {
        assert.commandWorked(this.st.s.getCollection(this.ns).insert(this.docs));
    };

    // Places whole chunks on the given shards. The collection starts entirely on the primary
    // (shard0); moving a whole chunk to another shard is safe because the recipient never owned it,
    // and chunks that should stay on the primary need no move. 'placement' maps a shard name to an
    // array of [minKey, maxKey] chunk bounds.
    const placeChunks = (placement) => {
        for (const [shardName, chunkBounds] of Object.entries(placement)) {
            if (shardName === this.shard0.shardName) {
                continue;
            }
            for (const bounds of chunkBounds) {
                assert.commandWorked(
                    this.st.s.adminCommand({
                        moveChunk: this.ns,
                        bounds,
                        to: shardName,
                        _waitForDelete: true,
                    }),
                );
            }
        }
    };

    // Assigns zones to shards and zone key ranges to the collection. 'shardZones' maps a shard name
    // to the zones it owns; 'zoneRanges' is an array of [minKey, maxKey, zone].
    const assignZones = (shardZones, zoneRanges) => {
        for (const [shardName, zones] of Object.entries(shardZones)) {
            for (const zone of zones) {
                assert.commandWorked(this.st.s.adminCommand({addShardToZone: shardName, zone}));
            }
        }
        for (const [min, max, zone] of zoneRanges) {
            assert.commandWorked(
                this.st.s.adminCommand({updateZoneKeyRange: this.ns, min, max, zone}),
            );
        }
    };

    it("does not balance when noBalance is set on the collection", () => {
        splitAt([-10, 0, 10, 20]);
        insertDocs();
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        assert.commandWorked(
            this.configDB.collections.update({_id: this.ns}, {$set: {noBalance: true}}),
        );
        runBalancer(this.st, 4);

        // All chunks remain on the primary shard.
        assertChunksOnShards(this.configDB, this.ns, {
            [this.shard0.shardName]: [
                [{x: MinKey}, {x: -10}],
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        });
        assert.eq(this.docs.length, this.shard0.getCollection(this.ns).count());
    });

    it("moves chunks to the shard that owns their zone", () => {
        splitAt([-10, 0, 10, 20]);
        insertDocs();
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        runBalancer(this.st, 4);

        const shardChunkBounds = {
            [this.shard0.shardName]: [[{x: MinKey}, {x: -10}]],
            [this.shard1.shardName]: [
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("does not allow removing the only shard a zone belongs to", () => {
        splitAt([-10, 0, 10, 20]);
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        assert.commandFailedWithCode(
            this.st.s.adminCommand({removeShardFromZone: this.shard0.shardName, zone: "zoneA"}),
            ErrorCodes.ZoneStillInUse,
        );
    });

    it("moves a zone's chunks when the zone is reassigned to another shard", () => {
        splitAt([-10, 0, 10, 20]);
        insertDocs();
        placeChunks({
            [this.shard1.shardName]: [
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        // Reassign zoneA from shard0 to shard1; its chunk must follow. shard1 never owned
        // [MinKey, -10) in this scenario, so receiving it is not rejected.
        moveZoneToShard(this.st, "zoneA", this.shard0, this.shard1);
        assertShardTags(this.configDB, {
            [this.shard0.shardName]: [],
            [this.shard1.shardName]: ["zoneB", "zoneA"],
            [this.shard2.shardName]: ["zoneC"],
        });

        runBalancer(this.st, 1);
        const shardChunkBounds = {
            [this.shard0.shardName]: [],
            [this.shard1.shardName]: [
                [{x: MinKey}, {x: -10}],
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("balances chunks within a zone", () => {
        splitAt([-10, 0, 10, 20]);
        insertDocs();
        placeChunks({
            [this.shard1.shardName]: [
                [{x: MinKey}, {x: -10}],
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        });
        assignZones(
            {
                [this.shard1.shardName]: ["zoneA", "zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        // Add shard0 to zoneB; the balancer must even out zoneB's chunks between shard0 and shard1.
        // shard0 owned [-10, 0) as a whole chunk before, so moving it back is not rejected.
        assert.commandWorked(
            this.st.s.adminCommand({addShardToZone: this.shard0.shardName, zone: "zoneB"}),
        );
        assertShardTags(this.configDB, {
            [this.shard0.shardName]: ["zoneB"],
            [this.shard1.shardName]: ["zoneB", "zoneA"],
            [this.shard2.shardName]: ["zoneC"],
        });

        runBalancer(this.st, 1);
        const shardChunkBounds = {
            [this.shard0.shardName]: [[{x: -10}, {x: 0}]],
            [this.shard1.shardName]: [
                [{x: MinKey}, {x: -10}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("moves chunks and documents when zones are reassigned across shards", () => {
        splitAt([-10, 0, 10, 20]);
        insertDocs();
        placeChunks({
            [this.shard1.shardName]: [
                [{x: MinKey}, {x: -10}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneB"],
                [this.shard1.shardName]: ["zoneA", "zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        // Rotate the zones across the shards. Every resulting migration is a whole-chunk move to a
        // shard that either never owned the range or owned it as an equal chunk.
        assert.commandWorked(
            this.st.s.adminCommand({removeShardFromZone: this.shard0.shardName, zone: "zoneB"}),
        );
        moveZoneToShard(this.st, "zoneC", this.shard2, this.shard0);
        moveZoneToShard(this.st, "zoneA", this.shard1, this.shard2);
        assertShardTags(this.configDB, {
            [this.shard0.shardName]: ["zoneC"],
            [this.shard1.shardName]: ["zoneB"],
            [this.shard2.shardName]: ["zoneA"],
        });

        runBalancer(this.st, 4);
        const shardChunkBounds = {
            [this.shard0.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
            [this.shard1.shardName]: [
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [[{x: MinKey}, {x: -10}]],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("splits and moves the affected chunk when a zone key range is reassigned", () => {
        splitAt([-10, 0, 10, 20]);
        insertDocs();
        placeChunks({
            [this.shard1.shardName]: [
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [[{x: MinKey}, {x: -10}]],
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneC"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneA"],
            },
            [
                [{x: MinKey}, {x: -10}, "zoneA"],
                [{x: -10}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        // Shrink zoneA and grow zoneB so [-20, -10) belongs to zoneB. The balancer splits shard2's
        // [MinKey, -10) at -20 and moves [-20, -10) to shard1, which never owned [MinKey, -10) here.
        updateZoneKeyRange(
            this.st,
            this.ns,
            "zoneA",
            [{x: MinKey}, {x: -10}],
            [{x: MinKey}, {x: -20}],
        );
        updateZoneKeyRange(this.st, this.ns, "zoneB", [{x: -10}, {x: 10}], [{x: -20}, {x: 10}]);
        runBalancer(this.st, 1);

        const shardChunkBounds = {
            [this.shard0.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
            [this.shard1.shardName]: [
                [{x: -20}, {x: -10}],
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [[{x: MinKey}, {x: -20}]], // no docs
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("moves chunks and docs so a zone ends up owning only empty chunks", () => {
        splitAt([-20, -10, 0, 10, 20]);
        insertDocs();
        placeChunks({
            [this.shard1.shardName]: [
                [{x: -20}, {x: -10}],
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
            ],
            [this.shard2.shardName]: [
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            [
                [{x: MinKey}, {x: -20}, "zoneA"],
                [{x: -20}, {x: 10}, "zoneB"],
                [{x: 10}, {x: MaxKey}, "zoneC"],
            ],
        );

        // Shrink zoneB and grow zoneC so the chunks holding data belong to zoneC. The balancer splits
        // shard1's [-20, -10) at -15 and moves [-15, -10), [-10, 0), [0, 10) to shard2, which never
        // owned those ranges here. zoneA (shard0) and zoneB (shard1) end up owning only empty chunks.
        updateZoneKeyRange(this.st, this.ns, "zoneB", [{x: -20}, {x: 10}], [{x: -20}, {x: -15}]);
        updateZoneKeyRange(
            this.st,
            this.ns,
            "zoneC",
            [{x: 10}, {x: MaxKey}],
            [{x: -15}, {x: MaxKey}],
        );
        runBalancer(this.st, 3);

        const shardChunkBounds = {
            [this.shard0.shardName]: [[{x: MinKey}, {x: -20}]], // no docs
            [this.shard1.shardName]: [[{x: -20}, {x: -15}]], // no docs
            [this.shard2.shardName]: [
                [{x: -15}, {x: -10}],
                [{x: -10}, {x: 0}],
                [{x: 0}, {x: 10}],
                [{x: 10}, {x: 20}],
                [{x: 20}, {x: MaxKey}],
            ],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
        assert.eq(this.docs.length, this.shard2.getCollection(this.ns).count());
    });
});
