/**
 * Checks that splitChunk, mergeChunks and mergeAllChunksOnShard do not run while a create
 * collection DDL coordinator has committed the new collection to the global catalog but not yet to
 * the shard catalog (parked on the hangAfterCommitOnGlobalCatalog failpoint).
 *
 * In that window the global catalog already reports the collection as sharded, while the shard
 * still considers it untracked and holds the default `allowChunkOperations: true`. The chunk
 * operation coordinators must base their preconditions on the shard's local metadata, so:
 *   - mergeAllChunksOnShard finds nothing to merge and returns without touching the critical
 *     section held by the create coordinator (SERVER-136129), and
 *   - splitChunk and mergeChunks are rejected by the shard with a retryable error, and succeed once
 *     the create coordinator has finished.
 *
 * @tags: [
 *   featureFlagCreateRenameNewSetAllowChunkOperationsBehavior,
 * ]
 */
import {configureFailPoint, configureFailPointForRS} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

// Zones defined before shardCollection make the create coordinator lay out several initial chunks,
// all of them on the single shard, so they are adjacent and mergeable.
const kZoneName = "zoneA";
const kZoneRanges = [
    {min: {x: 0}, max: {x: 10}},
    {min: {x: 10}, max: {x: 20}},
];
// The zone ranges above yield [MinKey, 0), [0, 10), [10, 20), [20, MaxKey).
const kNumInitialChunks = 4;

// Log id of "Coordinator failed, persisting abort reason".
const kCoordinatorAbortedLogId = 7418502;
// Log id of "Tripwire assertion".
const kTripwireLogId = 4457000;

describe("chunk operations between the global and shard catalog commits of create", function () {
    before(() => {
        this.st = new ShardingTest({shards: 1});
        this.dbName = "between_create_commits_db";

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created.
        for (const rs of [this.st.configRS, this.st.rs0]) {
            configureFailPointForRS(
                rs.nodes,
                "overrideHistoryWindowInSecs",
                {seconds: -10},
                "alwaysOn",
            );
        }

        assert.commandWorked(
            this.st.s.adminCommand({
                enableSharding: this.dbName,
                primaryShard: this.st.shard0.shardName,
            }),
        );
        assert.commandWorked(
            this.st.s.adminCommand({addShardToZone: this.st.shard0.shardName, zone: kZoneName}),
        );

        this.updateZoneRanges = (ns, zone) => {
            for (const range of kZoneRanges) {
                assert.commandWorked(
                    this.st.s.adminCommand({
                        updateZoneKeyRange: ns,
                        min: range.min,
                        max: range.max,
                        zone: zone,
                    }),
                );
            }
        };

        this.countChunks = (ns) => findChunksUtil.countChunksForNs(this.st.s.getDB("config"), ns);

        /**
         * Parks a `shardCollection` on `this.ns` between its global and shard catalog commits,
         * runs `runInWindow` while it is parked, and checks that the chunk layout did not change
         * and that no tripwire assertion fired on the shard. Then lets the create coordinator
         * finish and returns.
         */
        this.runBetweenCreateCommits = (runInWindow) => {
            const primary = this.st.rs0.getPrimary();
            const hangCreate = configureFailPoint(primary, "hangAfterCommitOnGlobalCatalog");

            const createShell = startParallelShell(
                funWithArgs(function (ns) {
                    assert.commandWorked(db.adminCommand({shardCollection: ns, key: {x: 1}}));
                }, this.ns),
                this.st.s.port,
            );

            try {
                hangCreate.wait();
                assert.eq(kNumInitialChunks, this.countChunks(this.ns));

                // The zones have served their purpose: mergeAllChunksOnShard never merges across
                // zone boundaries, so drop them now. They live on the config server, unaffected by
                // the critical section held on the shard.
                this.updateZoneRanges(this.ns, null);

                runInWindow();

                assert.eq(
                    kNumInitialChunks,
                    this.countChunks(this.ns),
                    "chunk operation must not commit before create commits on the shard catalog",
                );
                checkLog.checkContainsWithCountJson(primary, kTripwireLogId, {}, 0);
            } finally {
                hangCreate.off();
                createShell();
            }
        };

        /**
         * Runs `chunkOpCmd` in a parallel shell while create is parked, waits until the shard has
         * rejected it at least once, and checks that it succeeds once create has finished.
         */
        this.assertRejectedUntilCreateFinishes = (chunkOpCmd, operationType, expectedNumChunks) => {
            let chunkOpShell;

            this.runBetweenCreateCommits(() => {
                // The split and merge shardsvr commands return StaleConfig if the collection
                // isn't sharded, which causes the shard role to automatically refresh. The
                // refresh gets stuck in the CS held by the create coordinator, which is hung in
                // the failpoint. For that reason, we launch the command in a parallel shell and
                // wait for the log line to avoid deadlocking the test.
                chunkOpShell = startParallelShell(
                    funWithArgs(function (cmd) {
                        assert.commandWorkedOrFailedWithCode(
                            db.adminCommand(cmd),
                            ErrorCodes.ConflictingOperationInProgress,
                        );
                    }, chunkOpCmd),
                    this.st.s.port,
                );
                checkLog.containsJson(this.st.rs0.getPrimary(), kCoordinatorAbortedLogId, {
                    coordinatorId: {namespace: this.ns, operationType: operationType},
                    phase: "checkPreconditions",
                });
            });

            chunkOpShell();

            // Idempotent if the command already succeeded from the parallel shell.
            assert.commandWorked(this.st.s.adminCommand(chunkOpCmd));
            assert.eq(expectedNumChunks, this.countChunks(this.ns));
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
        this.updateZoneRanges(this.ns, kZoneName);
    });

    afterEach(() => {
        assert(this.st.s.getDB(this.dbName)[this.collName].drop());
    });

    it("mergeAllChunksOnShard is a no-op and does not touch the create critical section", () => {
        const cmd = {mergeAllChunksOnShard: this.ns, shard: this.st.shard0.shardName};
        this.runBetweenCreateCommits(() => assert.commandWorked(this.st.s.adminCommand(cmd)));
        assert.commandWorked(this.st.s.adminCommand(cmd));
        assert.eq(1, this.countChunks(this.ns));
    });

    it("splitChunk is rejected until create commits on the shard catalog", () => {
        this.assertRejectedUntilCreateFinishes(
            {split: this.ns, middle: {x: 5}},
            "splitChunk",
            kNumInitialChunks + 1,
        );
    });

    it("mergeChunks is rejected until create commits on the shard catalog", () => {
        this.assertRejectedUntilCreateFinishes(
            {mergeChunks: this.ns, bounds: [{x: 0}, {x: 20}]},
            "mergeChunks",
            kNumInitialChunks - 1,
        );
    });
});
