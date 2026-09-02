/**
 * Repro for a race between shard-side readConcern-wait and query-settings 'maxTimeMS' loosening:
 * mongos correctly resolves a looser effective 'maxTimeMS' from query settings and forwards it as
 * 'maxTimeMSOpOnly', but leaves the original, tighter client-supplied 'maxTimeMS' unmodified on
 * the wire. The shard's generic ingress deadline logic deliberately prefers the *shorter* of
 * 'maxTimeMS'/'maxTimeMSOpOnly' (so an internal hop's forwarded budget can never *extend* past
 * what the client asked for), so it picks the tight, unloosened value as the operation's initial
 * deadline - and a readConcern-wait (which runs before query settings are ever consulted, since
 * that happens inside the command's own execution) can therefore fail with a spurious
 * 'MaxTimeMSExpired' well before the intended, looser deadline, even though the query itself would
 * have completed comfortably within it.
 *
 * This is deterministically reproduced here by pausing oplog application on a shard's secondary,
 * issuing a secondary-routed read that needs to wait for that secondary to catch up to a recent
 * write, and resuming replication partway through - after the tight deadline would have elapsed,
 * but before the loose one would.
 *
 * Exercised for every command type that resolves query settings on the router and forwards them to
 * the shards, since each one builds its shard-bound command separately and therefore has to resolve
 * 'maxTimeMS' for forwarding on its own.
 *
 * Requires its own topology (a sharded cluster whose shard is a two-node replica set) so that oplog
 * application can be paused on a specific secondary.
 *
 * @tags: [
 *   requires_sharding,
 *   requires_replication,
 * ]
 */
import {waitForCurOpByFilter} from "jstests/libs/curop_helpers.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kTightMaxTimeMS = 200;
const kLooseMaxTimeMS = 5000;
const collName = "coll";

// Issues the secondary-routed, tight-client-maxTimeMS read from a fresh connection and asserts it
// succeeds - i.e. that the looser query-settings maxTimeMS actually governed. Run inside a
// parallel shell so it can block on the readConcern-wait while the main thread controls when the
// secondary is allowed to catch up. 'afterClusterTime' is what actually forces the readConcern-wait:
// without it, 'readConcern: majority' is satisfied immediately by whatever (possibly stale)
// majority-committed snapshot the secondary already has.
function runAndAssertSecondaryRoutedReadSucceeds(
    host,
    dbName,
    command,
    tightMaxTimeMS,
    afterClusterTime,
) {
    const conn = new Mongo(host);
    const testDb = conn.getDB(dbName);
    assert.commandWorked(
        testDb.runCommand({
            ...command,
            maxTimeMS: tightMaxTimeMS,
            readConcern: {level: "majority", afterClusterTime: afterClusterTime},
            $readPreference: {mode: "secondary"},
        }),
        "expected the query-settings-loosened maxTimeMS to govern, not the tight client-supplied " +
            "value",
    );
}

describe("query settings maxTimeMS across a shard-side readConcern wait", function () {
    before(function () {
        this.st = new ShardingTest({shards: 1, rs: {nodes: 2}, mongos: 1});
        this.dbName = jsTestName();
        this.db = this.st.s.getDB(this.dbName);
        this.coll = this.db.getCollection(collName);
        this.secondary = this.st.rs0.getSecondary();
        // Distinguishes the write each case performs to force its own readConcern-wait.
        this.nextId = 1;

        this.coll.drop();
        assert.commandWorked(this.coll.insert({_id: 0, a: 1}, {writeConcern: {w: 1}}));

        this.qsutils = new QuerySettingsUtils(this.db, collName);
        this.qsutils.removeAllQuerySettings();
    });

    after(function () {
        this.st.stop();
    });

    /**
     * Installs 'representativeQuery' with a loose 'maxTimeMS', then runs 'command' with a tighter
     * client-supplied one against a secondary that must first catch up, and asserts the loose value
     * governed. 'curOpField' identifies the command in the secondary's 'currentOp' output.
     */
    function assertLooseMaxTimeMSGoverns(ctx, {representativeQuery, command, curOpField}) {
        const {st, db, coll, secondary, qsutils, dbName} = ctx;

        // Install the query setting *before* pausing the secondary's oplog application:
        // 'setQuerySettings' itself is a majority-write-concern-dependent cluster-parameter write,
        // and would hang forever retrying against the (about to be) unreachable majority if the
        // secondary were already paused.
        qsutils.withQuerySettings(
            representativeQuery,
            {maxTimeMS: NumberLong(kLooseMaxTimeMS)},
            () => {
                // Pause oplog application on the secondary so it cannot advance past its current
                // optime, then perform a write that only needs to reach the primary. This makes the
                // secondary provably behind the primary's latest optime for as long as the pause
                // holds. Use 'w: 1' so this write itself doesn't block on the now-unreachable
                // majority.
                const pauseOplogApplication = configureFailPoint(secondary, "rsSyncApplyStop");
                let awaitShell;
                try {
                    // Wait for the fail point to actually start blocking before issuing the write
                    // below - otherwise a batch already in flight when the fail point was armed can
                    // still complete, letting the secondary observe the write despite the "pause".
                    pauseOplogApplication.wait();
                    assert.commandWorked(
                        coll.insert({_id: ctx.nextId++, a: 1}, {writeConcern: {w: 1}}),
                    );
                    // The secondary (paused above) cannot have applied this write yet, so requiring
                    // the read to observe it via 'afterClusterTime' forces a genuine
                    // readConcern-wait on the secondary. Read it back from the session (rather than
                    // the insert()'s convenience result object, which doesn't expose
                    // 'operationTime') to get a real Timestamp.
                    const afterClusterTime = db.getSession().getOperationTime();

                    awaitShell = startParallelShell(
                        funWithArgs(
                            runAndAssertSecondaryRoutedReadSucceeds,
                            st.s.host,
                            dbName,
                            command,
                            kTightMaxTimeMS,
                            afterClusterTime,
                        ),
                        st.s.port,
                    );

                    // Wait until the secondary-routed read has actually been running on the
                    // (paused) secondary for longer than the tight deadline - measuring the op's own
                    // reported runtime ties the wait directly to the operation's real elapsed time.
                    waitForCurOpByFilter(
                        secondary.getDB("admin"),
                        {[curOpField]: collName, microsecs_running: {$gt: kTightMaxTimeMS * 1000}},
                        {allUsers: true},
                    );
                } finally {
                    // Resuming replication is what lets the blocked read finally complete, so this
                    // is both the cleanup and a deliberate step of the scenario. Doing it here also
                    // guarantees the secondary is never left paused (which would hang every other
                    // majority-write-concern op in the cluster) if anything above throws.
                    pauseOplogApplication.off();
                }

                // Joined only after replication has resumed, mirroring the pattern in
                // 'query_settings_lost_update.js': the parallel shell's assertion is the real check,
                // and it can only complete once the fail point is released. Fails without the
                // router-side fix with MaxTimeMSExpired thrown from inside the shell.
                awaitShell();
            },
        );
    }

    // Note these cases are not independent: each pauses replication on the shared secondary and
    // resumes it before finishing, so they must run sequentially.
    it("is honored by find", function () {
        assertLooseMaxTimeMSGoverns(this, {
            representativeQuery: this.qsutils.makeFindQueryInstance({filter: {a: 1}}),
            command: {find: collName, filter: {a: 1}},
            curOpField: "command.find",
        });
    });

    it("is honored by distinct", function () {
        assertLooseMaxTimeMSGoverns(this, {
            representativeQuery: this.qsutils.makeDistinctQueryInstance({key: "a", query: {a: 1}}),
            command: {distinct: collName, key: "a", query: {a: 1}},
            curOpField: "command.distinct",
        });
    });

    it("is honored by aggregate", function () {
        assertLooseMaxTimeMSGoverns(this, {
            representativeQuery: this.qsutils.makeAggregateQueryInstance({
                pipeline: [{$match: {a: 1}}],
            }),
            command: {aggregate: collName, pipeline: [{$match: {a: 1}}], cursor: {}},
            curOpField: "command.aggregate",
        });
    });
});
