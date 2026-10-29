/**
 * Test that chunks and documents are moved correctly after zone changes for a hashed shard key.
 */
import {after, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {chunkBoundsUtil} from "jstests/sharding/libs/chunk_bounds_util.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";
import {
    assertChunksOnShards,
    assertDocsOnShards,
    assertShardTags,
    moveZoneToShard,
    runBalancer,
    updateZoneKeyRange,
} from "jstests/sharding/libs/zone_changes_util.js";

describe("hashed zone changes move chunks and documents correctly", function () {
    before(() => {
        this.st = new ShardingTest({shards: 3, other: {chunkSize: 1}});
        this.shard0 = this.st.shard0;
        this.shard1 = this.st.shard1;
        this.shard2 = this.st.shard2;
        this.configDB = this.st.s.getDB("config");
        this.shardKey = {x: "hashed"};

        // Boundaries of the three chunks that shardCollection presplits a hashed key into on a
        // three-shard cluster. They are fixed, so zone ranges can be expressed with them directly.
        this.bAB = {x: NumberLong("-3074457345618258602")};
        this.bBC = {x: NumberLong("3074457345618258602")};

        const bigString = "X".repeat(1024 * 1024); // 1MB
        this.docs = [{x: -25}, {x: -18}, {x: -5}, {x: -1}, {x: 5}, {x: 10}].map((d) => ({
            ...d,
            s: bigString,
        }));
        // One split inside each presplit chunk, chosen so every document lands in its own chunk.
        this.splitVals = [-18, -1, 10];
        this.dbCounter = 0;
    });

    after(() => {
        this.st.stop();
    });

    // Returns the collection's chunk bounds as an array of [minKey, maxKey], ordered by minKey.
    const sortedChunks = () =>
        findChunksUtil
            .findChunksByNs(this.configDB, this.ns)
            .sort({min: 1})
            .toArray()
            .map((c) => [c.min, c.max]);

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
        this.ns = `${this.dbName}.hashed`;
        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.shard0.shardName,
            }),
        );
        // A hashed shard key presplits into one chunk per shard, already at the boundaries that
        // divide the hashed space into the three zone regions.
        assert.commandWorked(
            this.st.s.adminCommand({shardCollection: this.ns, key: this.shardKey}),
        );

        // Split each presplit chunk once, in place, so there is one chunk per document. No chunk is
        // moved during setup, so no shard is left as a past owner of a range it no longer holds.
        for (const v of this.splitVals) {
            assert.commandWorked(
                this.st.s.adminCommand({split: this.ns, middle: {x: convertShardKeyToHashed(v)}}),
            );
        }
        assert.commandWorked(this.st.s.getCollection(this.ns).insert(this.docs));

        this.chunks = sortedChunks();
        assert.eq(6, this.chunks.length, "expected six chunks after splitting");

        // Confirm the chosen splits give one document per chunk; the scenarios rely on this.
        const docChunks = new Set();
        for (const doc of this.docs) {
            const hashedKey = {x: convertShardKeyToHashed(doc.x)};
            const idx = this.chunks.findIndex(([min, max]) =>
                chunkBoundsUtil.containsKey(hashedKey, min, max),
            );
            assert(idx >= 0, "no chunk contains document", {doc});
            docChunks.add(idx);
        }
        assert.eq(this.docs.length, docChunks.size, "expected one document per chunk");

        // The two lowest-hash chunks fall in the lowest presplit chunk, the next two in the middle
        // one, and the top two in the highest, so the ordered chunks group cleanly into three zones.
        this.zoneAChunks = this.chunks.slice(0, 2);
        this.zoneBChunks = this.chunks.slice(2, 4);
        this.zoneCChunks = this.chunks.slice(4, 6);
    });

    // Zone ranges aligned to the presplit boundaries: each zone owns two chunks / two documents.
    const presplitZoneRanges = () => [
        [{x: MinKey}, this.bAB, "zoneA"],
        [this.bAB, this.bBC, "zoneB"],
        [this.bBC, {x: MaxKey}, "zoneC"],
    ];

    // Places whole chunks on the given shards, moving each chunk from wherever it currently sits. A
    // chunk already on its target shard is left in place. Every scenario places each chunk on a
    // shard that did not previously own it, so these whole-chunk moves are never rejected.
    // 'placement' maps a shard name to an array of [minKey, maxKey] chunk bounds.
    const placeChunks = (placement) => {
        for (const [shardName, chunkBounds] of Object.entries(placement)) {
            for (const bounds of chunkBounds) {
                const current = findChunksUtil.findOneChunkByNs(this.configDB, this.ns, {
                    min: bounds[0],
                    max: bounds[1],
                });
                if (current.shard === shardName) {
                    continue;
                }
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
        // Gather every chunk onto the primary, then confirm the balancer leaves them there even
        // though the zones would otherwise pull them onto the other shards.
        placeChunks({[this.shard0.shardName]: this.chunks});
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        assert.commandWorked(
            this.configDB.collections.update({_id: this.ns}, {$set: {noBalance: true}}),
        );
        runBalancer(this.st, 4);

        // All chunks remain on the primary shard.
        assertChunksOnShards(this.configDB, this.ns, {[this.shard0.shardName]: this.chunks});
        assert.eq(this.docs.length, this.shard0.getCollection(this.ns).count());
    });

    it("moves chunks to the shard that owns their zone", () => {
        // Start with every chunk on the primary so the balancer must distribute them by zone.
        placeChunks({[this.shard0.shardName]: this.chunks});
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        runBalancer(this.st, 4);

        const shardChunkBounds = {
            [this.shard0.shardName]: this.zoneAChunks,
            [this.shard1.shardName]: this.zoneBChunks,
            [this.shard2.shardName]: this.zoneCChunks,
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("does not allow removing the only shard a zone belongs to", () => {
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        assert.commandFailedWithCode(
            this.st.s.adminCommand({removeShardFromZone: this.shard0.shardName, zone: "zoneA"}),
            ErrorCodes.ZoneStillInUse,
        );
    });

    it("moves a zone's chunks when the zone is reassigned to another shard", () => {
        placeChunks({
            [this.shard1.shardName]: this.zoneBChunks,
            [this.shard2.shardName]: this.zoneCChunks,
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        // Reassign zoneA from shard0 to shard1; its chunks must follow. shard1 never owned zoneA's
        // chunks in this scenario, so receiving them is not rejected.
        moveZoneToShard(this.st, "zoneA", this.shard0, this.shard1);
        assertShardTags(this.configDB, {
            [this.shard0.shardName]: [],
            [this.shard1.shardName]: ["zoneB", "zoneA"],
            [this.shard2.shardName]: ["zoneC"],
        });

        runBalancer(this.st, 2);
        const shardChunkBounds = {
            [this.shard0.shardName]: [],
            [this.shard1.shardName]: [...this.zoneAChunks, ...this.zoneBChunks],
            [this.shard2.shardName]: this.zoneCChunks,
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("balances chunks within a zone", () => {
        placeChunks({
            [this.shard1.shardName]: [...this.zoneAChunks, ...this.zoneBChunks],
            [this.shard2.shardName]: this.zoneCChunks,
        });
        assignZones(
            {
                [this.shard1.shardName]: ["zoneA", "zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        // Add shard0 to zoneB; the balancer must even out zoneB's two chunks between shard0 and
        // shard1. shard0 owned zoneB's chunks as whole chunks before, so moving one back is safe.
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
            [this.shard0.shardName]: [this.zoneBChunks[0]],
            [this.shard1.shardName]: [...this.zoneAChunks, this.zoneBChunks[1]],
            [this.shard2.shardName]: this.zoneCChunks,
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("moves chunks and documents when zones are reassigned across shards", () => {
        placeChunks({
            [this.shard1.shardName]: [...this.zoneAChunks, this.zoneBChunks[1]],
            [this.shard2.shardName]: this.zoneCChunks,
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneB"],
                [this.shard1.shardName]: ["zoneA", "zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        // Rotate the zones across the shards. Every resulting migration is a whole-chunk move to a
        // shard that either never owned the range in this scenario or owned it as an equal chunk.
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

        runBalancer(this.st, 5);
        const shardChunkBounds = {
            [this.shard0.shardName]: this.zoneCChunks,
            [this.shard1.shardName]: this.zoneBChunks,
            [this.shard2.shardName]: this.zoneAChunks,
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("splits and moves the affected chunk when a zone key range is reassigned", () => {
        placeChunks({
            [this.shard0.shardName]: this.zoneCChunks,
            [this.shard1.shardName]: this.zoneBChunks,
            [this.shard2.shardName]: this.zoneAChunks,
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneC"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneA"],
            },
            presplitZoneRanges(),
        );

        // Shrink zoneA and grow zoneB so a slice at the top of zoneA's highest chunk belongs to
        // zoneB. The split point lies strictly inside that chunk (its document sits well below it).
        const target = this.zoneAChunks[1];
        const splitPoint = {x: NumberLong(target[1].x - 5000)};
        assert(chunkBoundsUtil.containsKey(splitPoint, ...target));

        updateZoneKeyRange(
            this.st,
            this.ns,
            "zoneA",
            [{x: MinKey}, this.bAB],
            [{x: MinKey}, splitPoint],
        );
        updateZoneKeyRange(this.st, this.ns, "zoneB", [this.bAB, this.bBC], [splitPoint, this.bBC]);
        runBalancer(this.st, 1);

        // The balancer splits zoneA's highest chunk at splitPoint and moves the empty upper piece to
        // shard1. shard1 never owned that wider chunk in this scenario, so the move is not rejected.
        const shardChunkBounds = {
            [this.shard0.shardName]: this.zoneCChunks,
            [this.shard1.shardName]: [...this.zoneBChunks, [splitPoint, this.bAB]],
            [this.shard2.shardName]: [this.zoneAChunks[0], [target[0], splitPoint]],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
    });

    it("moves chunks and docs so a zone ends up owning only empty chunks", () => {
        placeChunks({
            [this.shard1.shardName]: this.zoneBChunks,
            [this.shard2.shardName]: this.zoneCChunks,
        });
        assignZones(
            {
                [this.shard0.shardName]: ["zoneA"],
                [this.shard1.shardName]: ["zoneB"],
                [this.shard2.shardName]: ["zoneC"],
            },
            presplitZoneRanges(),
        );

        // Shrink zoneA and zoneB to empty slivers at the tops of their highest chunks, and grow
        // zoneC over the vacated regions so every document ends up in zoneC. Both split points lie
        // strictly inside their chunks, above every document those chunks hold.
        const splitA = {x: NumberLong(this.zoneAChunks[1][1].x - 5000)};
        const splitB = {x: NumberLong(this.zoneBChunks[1][1].x - 5000)};
        assert(chunkBoundsUtil.containsKey(splitA, ...this.zoneAChunks[1]));
        assert(chunkBoundsUtil.containsKey(splitB, ...this.zoneBChunks[1]));

        updateZoneKeyRange(this.st, this.ns, "zoneA", [{x: MinKey}, this.bAB], [splitA, this.bAB]);
        updateZoneKeyRange(this.st, this.ns, "zoneB", [this.bAB, this.bBC], [splitB, this.bBC]);
        assert.commandWorked(
            this.st.s.adminCommand({
                updateZoneKeyRange: this.ns,
                min: {x: MinKey},
                max: splitA,
                zone: "zoneC",
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({
                updateZoneKeyRange: this.ns,
                min: this.bAB,
                max: splitB,
                zone: "zoneC",
            }),
        );
        runBalancer(this.st, 4);

        // Every document-bearing chunk moves to shard2, which never owned those ranges in this
        // scenario. shard0 and shard1 are donors that keep only an empty sliver each.
        const shardChunkBounds = {
            [this.shard0.shardName]: [[splitA, this.bAB]], // no docs
            [this.shard1.shardName]: [[splitB, this.bBC]], // no docs
            [this.shard2.shardName]: [
                this.zoneAChunks[0],
                [this.zoneAChunks[1][0], splitA],
                this.zoneBChunks[0],
                [this.zoneBChunks[1][0], splitB],
                ...this.zoneCChunks,
            ],
        };
        assertChunksOnShards(this.configDB, this.ns, shardChunkBounds);
        assertDocsOnShards(this.st, this.ns, shardChunkBounds, this.docs, this.shardKey);
        assert.eq(this.docs.length, this.shard2.getCollection(this.ns).count());
    });
});
