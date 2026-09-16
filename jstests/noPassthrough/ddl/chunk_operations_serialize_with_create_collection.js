/**
 * Checks that splitChunk, mergeChunks and mergeAllChunksOnShard serialize with a concurrent create
 * collection DDL coordinator. While the create coordinator still holds the critical section (parked
 * on the createCollectionHangBeforeExitCriticalSection failpoint) no chunk operation may commit,
 * and each of them must complete successfully once the critical section is released.
 *
 * @tags: [
 *   requires_fcv_90,
 * ]
 */
import {configureFailPoint, configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

// The merge operations need the collection to already have several chunks by the time the create
// coordinator holds the critical section. Zones defined before shardCollection make the create
// coordinator lay out the initial chunks along the zone boundaries. A single-shard cluster keeps
// every chunk on the same shard, so all of them are adjacent and mergeable.
const kZoneName = "zoneA";
const kZoneRanges = [
    {min: {x: 0}, max: {x: 10}},
    {min: {x: 10}, max: {x: 20}},
];
// The zone ranges above yield [MinKey, 0), [0, 10), [10, 20), [20, MaxKey).
const kInitialChunkBounds = [
    [MinKey, 0],
    [0, 10],
    [10, 20],
    [20, MaxKey],
];
const kNumInitialChunks = kInitialChunkBounds.length;

// How long the chunk operation is left running against the held critical section before concluding
// it is not making progress.
const kBlockedWindowMS = 3000;

describe("chunk operations serialize with the create collection critical section", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1});
        this.dbName = "serialize_with_create_db";

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created;
        // otherwise it returns early before reaching the check this test exercises.
        configureFailPointForRS(
            this.st.configRS.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: -10},
            "alwaysOn",
        );
        configureFailPointForRS(
            this.st.rs0.nodes,
            "overrideHistoryWindowInSecs",
            {seconds: -10},
            "alwaysOn",
        );

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );

        assert.commandWorked(
            this.st.s.adminCommand({addShardToZone: this.st.shard0.shardName, zone: kZoneName}),
        );

        this.removeZoneRanges = (ns) => {
            for (const range of kZoneRanges) {
                assert.commandWorked(
                    this.st.s.adminCommand({
                        updateZoneKeyRange: ns,
                        min: range.min,
                        max: range.max,
                        zone: null,
                    }),
                );
            }
        };

        this.countChunks = (ns) =>
            findChunksUtil.findChunksByNs(this.st.s.getDB("config"), ns).itcount();

        this.chunkBounds = (ns) =>
            findChunksUtil
                .findChunksByNs(this.st.s.getDB("config"), ns)
                .sort({min: 1})
                .toArray()
                .map((chunk) => [chunk.min.x, chunk.max.x]);

        /**
         * Parks a `shardCollection` on `this.ns` right before it exits the critical section, runs
         * `chunkOpCmd` through mongos while the critical section is still held, and asserts that:
         *   - the chunk operation does not commit while the critical section is held, and
         *   - it completes successfully once the critical section is released, leaving
         *     `expectedNumChunksAfterwards` chunks behind.
         */
        this.assertSerializesWithCreate = (chunkOpCmd, expectedNumChunksAfterwards) => {
            const hangCreate = configureFailPoint(
                this.st.rs0.getPrimary(),
                "createCollectionHangBeforeExitCriticalSection",
            );

            const createShell = startParallelShell(
                funWithArgs(function (ns) {
                    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
                }, this.ns),
                this.st.s.port,
            );

            let chunkOpShell = undefined;

            try {
                hangCreate.wait();

                assert.eq(kNumInitialChunks, this.countChunks(this.ns));
                assert.eq(
                    kInitialChunkBounds,
                    this.chunkBounds(this.ns),
                    "unexpected initial chunk layout, the test's split/merge bounds are stale",
                );

                // The zones have served their purpose. Drop them now that the chunks exist:
                // mergeAllChunksOnShard never merges across zone boundaries, and with one zone per
                // chunk it would have nothing to do. Zone ranges live on the config server, so this
                // is unaffected by the critical section held on the shard.
                this.removeZoneRanges(this.ns);

                chunkOpShell = startParallelShell(
                    funWithArgs(function (cmd) {
                        // TODO (SERVER-133735): with setAllowChunkOperations: false, the chunk
                        // operations will likely fail instead of serializing.
                        assert.commandWorked(db.adminCommand(cmd));
                    }, chunkOpCmd),
                    this.st.s.port,
                );

                // The chunk operation must wait for the critical section instead of committing or
                // giving up.
                sleep(kBlockedWindowMS);
                assert.eq(
                    kNumInitialChunks,
                    this.countChunks(this.ns),
                    "chunk operation must not commit while the create critical section is held",
                );
            } finally {
                hangCreate.off();
                createShell();
                if (chunkOpShell) {
                    chunkOpShell();
                }
            }

            // TODO (SERVER-133735): with setAllowChunkOperations: false, the chunk operations will
            // likely fail instead of serializing.
            assert.eq(expectedNumChunksAfterwards, this.countChunks(this.ns));
        };
    });

    after(() => {
        assert.commandWorked(
            this.st.s.adminCommand({
                removeShardFromZone: this.st.shard0.shardName,
                zone: kZoneName,
            }),
        );
        this.st.stop();
    });

    beforeEach(() => {
        // Fresh namespace per test: each test needs a create coordinator of its own.
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;

        // Pre-define the zones so that shardCollection lays out several initial chunks.
        for (const range of kZoneRanges) {
            assert.commandWorked(
                this.st.s.adminCommand({
                    updateZoneKeyRange: this.ns,
                    min: range.min,
                    max: range.max,
                    zone: kZoneName,
                }),
            );
        }
    });

    afterEach(() => {
        assert.commandWorkedOrFailedWithCode(
            this.st.s.getDB(this.dbName).runCommand({drop: this.collName}),
            ErrorCodes.NamespaceNotFound,
        );
        // No-op unless the test bailed out before the helper removed them.
        this.removeZoneRanges(this.ns);
    });

    it("serializes splitChunk with the create collection critical section", () => {
        this.assertSerializesWithCreate({split: this.ns, middle: {x: 5}}, kNumInitialChunks + 1);
    });

    it("serializes mergeChunks with the create collection critical section", () => {
        this.assertSerializesWithCreate(
            {mergeChunks: this.ns, bounds: [{x: 0}, {x: 20}]},
            kNumInitialChunks - 1,
        );
    });

    it("serializes mergeAllChunksOnShard with the create collection critical section", () => {
        this.assertSerializesWithCreate(
            {mergeAllChunksOnShard: this.ns, shard: this.st.shard0.shardName},
            1,
        );
    });
});
