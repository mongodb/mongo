/**
 * Tests $unionWith stage of aggregate command concurrently with killOp. Ensures that all cursors
 * opened on behalf of the $unionWith are killed when interrupted.
 *
 * @tags: [
 *   uses_curop_agg_stage,
 *   requires_getmore,
 *   # The 'killOp' state uses the killOp cmd, which is incompatible with txns, and a getMore in
 *   # same state. As a result the getMore will be run outside of a txn.
 *   uses_getmore_outside_of_transaction,
 *   # This test relies on aggregations returning specific batch-sized responses.
 *   assumes_no_implicit_cursor_exhaustion,
 * ]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_out_interrupt_cleanup.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.commentStr = "agg_unionWith_interrupt_cleanup";

    $config.states.aggregate = function aggregate(db, collName) {
        // Here we consistenly union with the same namespace to benefit from the sharded collection
        // setup that may have been done in sharded passthroughs.
        // TODO SERVER-46251 use multiple namespaces.
        let response = db[collName].runCommand({
            aggregate: collName,
            pipeline: [{$unionWith: {coll: collName, pipeline: [{$unionWith: collName}]}}],
            comment: this.commentStr,
            // Use a small batch size to ensure these operations open up a cursor and use multiple
            // getMores. We want to give coverage to interrupting the getMores as well.
            cursor: {batchSize: this.numDocs / 4},
        });
        // Keep iterating the cursor until we exhaust it or we are interrupted.
        while (response.ok && response.cursor.id != 0) {
            response = db[collName].runCommand({getMore: response.cursor.id, collection: collName});
        }
        if (!response.ok) {
            // If the interrupt happens just as the cursor is being checked back in, the cursor will
            // be killed without failing the operation. When this happens, the next getMore will
            // fail with CursorNotFound.
            assert.contains(response.code, interruptedQueryErrors, response);
        }
    };

    $config.states.killOp = function killOp(db, collName) {
        // The aggregate command could be running different sub-aggregates internally depending on
        // which stage of execution it is in. So we rely on the comment to detect which operations
        // are eligible to be interrupted, and interrupt those.
        this.killOpsMatchingFilter(db, {
            $and: [
                {active: true},
                {
                    $or: [{"command.comment": this.commentStr}, {"cursor.originatingCommand.comment": this.commentStr}],
                },
            ],
        });
    };

    $config.teardown = function teardown(db, collName, cluster) {
        // Ensure that no operations, cursors, or sub-operations are left active. After
        // SERVER-46255, We normally expect all operations to be cleaned up safely, but there are
        // race conditions or possible network blips where the kill won't arrive as expected. We
        // don't want to block the interrupt thread or the operation itself to wait around to make
        // sure everything dies correctly, so we just rely on cursor timeouts or session reaps to
        // cover these rare cases. Here we make sure everything is cleaned up so we avoid hogging
        // resources for future tests.
        this.killOpsMatchingFilter(db, {
            $and: [
                {active: true},
                {
                    $or: [{"command.comment": this.commentStr}, {"cursor.originatingCommand.comment": this.commentStr}],
                },
            ],
        });

        const killCursors = (db) => {
            const adminDB = db.getSiblingDB("admin");

            const curOpCursor = adminDB.aggregate([
                {$currentOp: {idleCursors: true, localOps: true}},
                {$match: {"cursor.originatingCommand.comment": this.commentStr}},
                {$project: {"cursor.cursorId": 1}},
            ]);

            while (curOpCursor.hasNext()) {
                let result = curOpCursor.next();
                if (result.cursor) {
                    const cursorId = result.cursor.cursorId;
                    jsTestLog(`Killing cursor: ${cursorId}, database ${db.getName()}`);
                    assert.commandWorked(db.runCommand({killCursors: collName, cursors: [cursorId]}));
                }
            }
        };

        cluster.executeOnMongodNodes(killCursors);
        cluster.executeOnMongosNodes(killCursors);
        if (cluster.isSharded()) {
            cluster.executeOnConfigNodes(killCursors);
        }

        const remainingOps = () =>
            db
                .getSiblingDB("admin")
                .aggregate([
                    {$currentOp: {idleCursors: true}},
                    // Look for any trace of state that wasn't cleaned up.
                    {
                        $match: {
                            $or: [
                                // The originating aggregation or a sub-aggregation still active.
                                {"command.comment": this.commentStr},
                                // An idle cursor left around.
                                {"cursor.originatingCommand.comment": this.commentStr},
                            ],
                        },
                    },
                ])
                .toArray();
        assert.soon(
            () => remainingOps().length == 0,
            () => "tried to kill cursors but they're still alive\n" + tojson(remainingOps()),
        );
    };

    return $config;
});
