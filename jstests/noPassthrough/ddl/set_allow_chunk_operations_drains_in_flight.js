/**
 * Checks that disabling allowChunkOperations on a shard waits for in-flight split, mergeChunks and
 * mergeAllChunksOnShard operations to finish before it returns. To make the wait visible, the test
 * pauses the chunk-operation coordinator at a failpoint and runs the shard command in a parallel
 * shell that writes a marker when it returns. The marker must be absent while the coordinator is
 * paused and present once it is released.
 *
 * @tags: [
 *   featureFlagAuthoritativeShardsDDL,
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";
import {
    getConfigSvrAllowChunkOperations,
    getShardAllowChunkOperations,
    setAllowChunkOperations,
} from "jstests/sharding/libs/set_allow_chunk_operations_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Collection the parallel shell writes to when the shard command returns. It lives on the config
// server so the test can see it.
const kMarkerDb = "drain_marker_db";
const kMarkerColl = "set_allow_chunk_ops_marker";

describe("setAllowChunkOperations drains in-flight chunk coordinators", function () {
    before(() => {
        this.st = new ShardingTest({shards: 2});
        this.dbName = "drain_in_flight_db";

        // Let mergeAllChunksOnShard merge chunks no matter how recently they were created.
        configureFailPoint(this.st.configRS.getPrimary(), "overrideHistoryWindowInSecs", {seconds: -10}, "alwaysOn");
        configureFailPoint(this.st.rs0.getPrimary(), "overrideHistoryWindowInSecs", {seconds: -10}, "alwaysOn");
        configureFailPoint(this.st.rs1.getPrimary(), "overrideHistoryWindowInSecs", {seconds: -10}, "alwaysOn");

        assert.commandWorked(
            this.st.s.adminCommand({enableSharding: this.dbName, primaryShard: this.st.shard0.shardName}),
        );
    });

    after(() => {
        this.st.stop();
    });

    beforeEach(() => {
        this.collName = "coll_" + new ObjectId().str;
        this.ns = this.dbName + "." + this.collName;
        assert.commandWorked(this.st.s.adminCommand({shardCollection: this.ns, key: {x: 1}}));

        this.markerColl = this.st.configRS.getPrimary().getDB(kMarkerDb).getCollection(kMarkerColl);
        this.markerColl.drop();
    });

    afterEach(() => {
        setAllowChunkOperations(this.st, this.ns, true);
        assert.commandWorked(this.st.s.getDB(this.dbName).runCommand({drop: this.collName}));
    });

    /**
     * Pauses shard0's chunk op coordinator at the failpoint, starts `opFn` in a parallel shell,
     * runs the config-server half of the toggle (which does not block), then runs the shard half in
     * a second parallel shell. Checks that the shard command stays blocked while the coordinator is
     * paused, then releases it and checks the command returns and the flag is set on both sides.
     *
     * `opFn` runs in the parallel shell with (mongosHost, ns); callers add any extra args.
     * `coordinatorType` is the operation type stored in the coordinator's state document.
     */
    this.runDrainScenario = (opFn, opArgs, coordinatorType) => {
        const shard0Primary = this.st.rs0.getPrimary();
        const failpoint = configureFailPoint(shard0Primary, "hangBeforeRunningCoordinatorInstance");

        // Start the chunk operation. Its coordinator pauses at the failpoint on shard0.
        const opShell = startParallelShell(funWithArgs(opFn, this.st.s.host, this.ns, ...opArgs), this.st.s.port);

        // Wait for the coordinator to pause at the failpoint. Polling its state document instead
        // would be racy.
        failpoint.wait();

        // Run the config-server half directly; it does not block. The shard half is the one that
        // drains in-flight operations, so we run it separately to watch it block.
        assert.commandWorked(
            this.st.configRS.getPrimary().adminCommand({
                _configsvrSetAllowChunkOperations: this.ns,
                allowChunkOperations: false,
                writeConcern: {w: "majority"},
                lsid: {id: UUID()},
                txnNumber: NumberLong(0),
            }),
        );

        // Run the shard command in a parallel shell. It must block until the coordinator finishes.
        // When it returns, the shell writes the marker.
        const shardsvrShell = startParallelShell(
            funWithArgs(
                function (shard0Host, ns, ignoredShardVersion, primaryShardId, markerHost, markerDb, markerColl) {
                    const shardConn = new Mongo(shard0Host);
                    assert.commandWorked(
                        shardConn.adminCommand({
                            _shardsvrSetAllowChunkOperations: ns,
                            allowChunkOperations: false,
                            // Required by the command's IDL: the database's primary shard.
                            primaryShardId: primaryShardId,
                            writeConcern: {w: "majority"},
                            lsid: {id: UUID()},
                            txnNumber: NumberLong(0),
                            shardVersion: ignoredShardVersion,
                        }),
                    );
                    new Mongo(markerHost).getDB(markerDb).getCollection(markerColl).insertOne({_id: "done"});
                },
                shard0Primary.host,
                this.ns,
                ShardVersioningUtil.kIgnoredShardVersion,
                this.st.shard0.shardName,
                this.st.configRS.getPrimary().host,
                kMarkerDb,
                kMarkerColl,
            ),
            this.st.s.port,
        );

        // While the coordinator is paused the shard command must stay blocked, so the marker must
        // be absent. Use try/finally so a failed assertion still releases the failpoint, otherwise
        // the parallel shells would hang at teardown.
        try {
            sleep(1000);
            assert.eq(
                null,
                this.markerColl.findOne({_id: "done"}),
                "_shardsvrSetAllowChunkOperations returned while the " +
                    coordinatorType +
                    " coordinator was still in flight -- drain contract violated",
            );
        } finally {
            // Release the failpoint. The coordinator may succeed or fail with
            // ConflictingOperationInProgress; either is fine. All we need is for it to finish so
            // the drain can return.
            failpoint.off();
        }

        opShell();
        shardsvrShell();

        assert.soon(
            () => this.markerColl.findOne({_id: "done"}) !== null,
            "shardsvrSetAllowChunkOperations parallel shell never wrote completion marker",
            30 * 1000,
        );

        // Final checks: the flag is set on both sides, later chunk operations are rejected, and the
        // coordinator state document is gone.
        assert.eq(false, getConfigSvrAllowChunkOperations(this.st, this.ns));
        assert.eq(false, getShardAllowChunkOperations(this.st.shard0, this.ns));

        assert.commandFailedWithCode(
            this.st.s.adminCommand({split: this.ns, middle: {x: 999}}),
            ErrorCodes.ConflictingOperationInProgress,
        );

        assert.eq(
            null,
            shard0Primary
                .getDB("config")
                .getCollection("system.sharding_ddl_coordinators")
                .findOne({"_id.namespace": this.ns, "_id.operationType": coordinatorType}),
            coordinatorType + " state doc was not removed after the operation finished",
        );
    };

    it("drains an in-flight splitChunk coordinator", () => {
        this.runDrainScenario(
            function splitOp(mongosHost, ns) {
                new Mongo(mongosHost).getDB("admin").runCommand({split: ns, middle: {x: 0}});
            },
            [],
            "splitChunk",
        );
    });

    it("drains an in-flight mergeChunks coordinator", () => {
        // Pre-split into three chunks so a merge has something to do.
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));

        this.runDrainScenario(
            function mergeOp(mongosHost, ns) {
                new Mongo(mongosHost).getDB("admin").runCommand({mergeChunks: ns, bounds: [{x: MinKey}, {x: 10}]});
            },
            [],
            "mergeChunks",
        );
    });

    it("drains an in-flight mergeAllChunksOnShard coordinator", () => {
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 0}}));
        assert.commandWorked(this.st.s.adminCommand({split: this.ns, middle: {x: 10}}));

        this.runDrainScenario(
            function mergeAllOp(mongosHost, ns, shardName) {
                new Mongo(mongosHost).getDB("admin").runCommand({mergeAllChunksOnShard: ns, shard: shardName});
            },
            [this.st.shard0.shardName],
            "mergeAllChunks",
        );
    });
});
