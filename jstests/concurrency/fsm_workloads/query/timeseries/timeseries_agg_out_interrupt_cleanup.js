/**
 * Tests $out stage of aggregate command with time-series collections concurrently with killOp.
 * Ensures that all the temporary collections created during the aggregate command are deleted and
 * that all buckets collection have a corresponding view. This workloads extends
 * 'agg_out_interrupt_cleanup'.
 *
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_stepdowns,
 *   does_not_support_transactions,
 *   uses_curop_agg_stage,
 *   # TODO Undenylist (SERVER-38852).
 *   assumes_against_mongod_not_mongos,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_out_interrupt_cleanup.js";
import {areViewlessTimeseriesEnabled} from "jstests/core/timeseries/libs/viewless_timeseries_util.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    const timeFieldName = "time";
    const metaFieldName = "tag";
    const numDocs = 100;

    $config.states.aggregate = function aggregate(db, collName) {
        // drop the view to ensure that each time a buckets collection is made, the view will also
        // be made or both be destroyed.
        assert(db["interrupt_temp_out"].drop());
        // $out to the same collection so that concurrent aggregate commands would cause congestion.
        db[collName].runCommand({
            aggregate: collName,
            pipeline: [
                {
                    $out: {
                        db: db.getName(),
                        coll: "interrupt_temp_out",
                        timeseries: {timeField: timeFieldName, metaField: metaFieldName},
                    },
                },
            ],
            cursor: {},
        });
    };

    $config.states.killOp = function killOp(db, collName) {
        // The aggregate command could be running different commands internally (renameCollection,
        // insertDocument, etc.) depending on which stage of execution it is in. So, get all the
        // operations that are running against the input, output or temp collections.
        const filter = {
            op: "command",
            active: true,
            $or: [
                {"ns": db.getName() + ".interrupt_temp_out"}, // For the view.
                {"ns": db.getName() + "." + collName}, // For input collection.
                // For the tmp collection.
                {"ns": {$regex: "^" + db.getName() + "\.system.buckets\.tmp\.agg_out.*"}},
            ],
            "command.drop": {
                $exists: false,
            }, // Exclude 'drop' command from the filter to make sure that we don't kill the the
            // drop command which is responsible for dropping the temporary collection in the
            // destructor. This won't prevent any drop commands run internally (with the same
            // operation context) by $out, such as in renameCollection.
        };
        if (TestData.testingReplicaSetEndpoint) {
            // The sharding DDL operations do not have opid.
            filter["$and"] = [
                {desc: {$ne: "CreateCollectionCoordinator"}},
                {desc: {$ne: "DropCollectionCoordinator"}},
                {desc: {$ne: "DropParticipantInstance"}},
                {desc: {$ne: "RenameCollectionCoordinator"}},
                {desc: {$ne: "RenameParticipantInstance"}},
            ];
        }
        $super.data.killOpsMatchingFilter(db, filter);
    };

    $config.teardown = function teardown(db) {
        if (TestData.testingReplicaSetEndpoint) {
            // When testing replica set endpoint, the temporary collection might not get deleted.
            // Instead, it will be cleaned up on the next step up.
            return;
        }

        const collNames = db.getCollectionNames();
        const temporaryAggCollections = collNames.filter((coll) => coll.includes("tmp.agg_out"));
        assert.eq(
            temporaryAggCollections.length,
            0,
            "Temporary agg collection left behind: " + tojson(temporaryAggCollections),
        );

        // TODO SERVER-101784 remove these checks once only viewless timeseries exist.
        if (!areViewlessTimeseriesEnabled(db)) {
            // Check that if the buckets collection exists then the view also exists
            const bucketCollectionPresent = collNames.includes("system.buckets.interrupt_temp_out");
            const viewPresent = collNames.includes("interrupt_temp_out");
            assert(
                !bucketCollectionPresent || viewPresent,
                "View must be present if bucket collection is present: " + tojson(collNames),
            );
        }
    };

    /**
     * Create a time-series collection and insert 100 documents.
     */
    $config.setup = function setup(db, collName, cluster) {
        db[collName].drop();
        assert.commandWorked(
            db.createCollection(collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
        );
        const docs = [];
        for (let i = 0; i < numDocs; ++i) {
            docs.push({
                [timeFieldName]: ISODate(),
                [metaFieldName]: this.tid * numDocs + i,
            });
        }
        assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));
    };

    return $config;
});
