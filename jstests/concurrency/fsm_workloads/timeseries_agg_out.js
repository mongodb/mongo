/**
 * This test runs many concurrent aggregations using $out, writing to the same time-series
 * collection. While this is happening, other threads may be creating or dropping indexes, changing
 * the collection options, or sharding the collection. We expect an aggregate with a $out stage to
 * fail if another client executed one of these changes between the creation of $out's temporary
 * collection and the eventual rename to the target collection.
 *
 * Unfortunately, there aren't very many assertions we can make here, so this is mostly to test that
 * the server doesn't deadlock or crash, and that temporary namespaces are cleaned up.
 *
 * @tags: [
 *   requires_timeseries,
 *   does_not_support_transactions,
 *   does_not_support_stepdowns,
 *   requires_fcv_71,
 *   featureFlagAggOutTimeseries,
 *   # `convertToCapped` is not supported in serverless.
 *   command_not_supported_in_serverless,
 * ]
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from 'jstests/concurrency/fsm_workloads/agg_out.js';

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';
    const numDocs = 100;
    $config.data.outputCollName = 'timeseries_agg_out';
    $config.data.shardKey = {[metaFieldName]: 1};

    /**
     * Runs an aggregate with a $out with time-series into '$config.data.outputCollName'.
     */
    $config.states.query = function query(db, collName) {
        jsTestLog(`Running query: coll=${collName} out=${this.outputCollName}`);
        const res = db[collName].runCommand({
            aggregate: collName,
            pipeline: [
                {$set: {"time": new Date()}},
                {
                    $out: {
                        db: db.getName(),
                        coll: this.outputCollName,
                        timeseries: {timeField: timeFieldName, metaField: metaFieldName}
                    }
                }
            ],
            cursor: {}
        });

        const allowedErrorCodes = [
            // indexes of target collection changed during processing.
            ErrorCodes.CommandFailed,
            // $out is not supported to an existing *sharded* output collection
            ErrorCodes.IllegalOperation,
            // namespace is capped so it can't be used for $out.
            17152,
            // $out collection cannot be sharded.
            ErrorCodes.NamespaceCannotBeSharded,
            // $out tries to create a view when a buckets collection already exists. This error is
            // not caught because the view is being dropped by a previous thread.
            ErrorCodes.NamespaceExists,
            // $out can't be executed while there is a move primary in progress
            ErrorCodes.MovePrimaryInProgress,
            // TODO SERVER-87422 potentially remove this error since there are no concurrent drops
            ErrorCodes.NamespaceNotFound,
        ];
        assert.commandWorkedOrFailedWithCode(res, allowedErrorCodes);
        if (res.ok) {
            const cursor = new DBCommandCursor(db, res);
            assert.eq(0, cursor.itcount());  // No matter how many documents were in the
            // original input stream, $out should never return any results.
        }
    };

    /**
     * Changes the 'expireAfterSeconds' value for the time-series collection.
     */
    $config.states.collMod = function collMod(db, unusedCollName) {
        let expireAfterSeconds = "off";
        if (Random.rand() < 0.5) {
            // Change the expireAfterSeconds
            expireAfterSeconds = Random.rand();
        }

        jsTestLog(`Running collMod: coll=${this.outputCollName} expireAfterSeconds=${
            expireAfterSeconds}`);
        assert.commandWorkedOrFailedWithCode(
            db.runCommand({collMod: this.outputCollName, expireAfterSeconds: expireAfterSeconds}),
            [ErrorCodes.ConflictingOperationInProgress, ErrorCodes.NamespaceNotFound]);
    };

    /**
     * Convert the collection to capped.
     */
    $config.states.convertToCapped = function convertToCapped(db, unusedCollName) {
        if (isMongos(db)) {
            return;  // convertToCapped can't be run against a mongos.
        }
        jsTestLog(`Running convertToCapped: coll=${this.outputCollName}`);
        assert.commandFailedWithCode(
            db.runCommand({convertToCapped: this.outputCollName, size: 100000}),
            [ErrorCodes.CommandNotSupportedOnView, ErrorCodes.NamespaceNotFound]);
    };

    $config.teardown = function teardown(db) {
        const collNames = db.getCollectionNames();

        // Ensure that for the buckets collection there is a corresponding view.
        assert(!(collNames.includes('system.buckets.timeseries_agg_out') &&
                 !collNames.includes('timeseries_agg_out')));
    };

    /**
     * Create a time-series collection and insert 100 documents.
     */
    $config.setup = function setup(db, collName, cluster) {
        db[collName].drop();
        assert.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        const docs = [];
        for (let i = 0; i < numDocs; ++i) {
            docs.push({
                [timeFieldName]: ISODate(),
                [metaFieldName]: (this.tid * numDocs) + i,
            });
        }
        assert.commandWorked(db.runCommand({insert: collName, documents: docs, ordered: false}));

        if (isMongos(db)) {
            this.shards = Object.keys(cluster.getSerializedCluster().shards);
        }
    };

    return $config;
});
