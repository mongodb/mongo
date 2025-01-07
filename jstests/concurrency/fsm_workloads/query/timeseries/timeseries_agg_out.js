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
 *   # `convertToCapped` is not supported in serverless.
 *   command_not_supported_in_serverless,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */
import {interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from 'jstests/concurrency/fsm_workloads/query/agg/agg_out.js';

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

        let allowedErrorCodes = [
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
            // This error is returned if output collection doesn't exist when $out first fetches
            // collection options, but then created by another thread before $out finished
            // timeseries options validation.
            7268700
        ];

        // TODO (SERVER-88275) a moveCollection can cause the original collection to be dropped and
        // re-created with a different uuid, causing the aggregation to fail with QueryPlannedKilled
        // when the mongos is fetching data from the shard using getMore(). Remove
        // the interruptedQueryErrors from allowedErrorCodes once this bug is being addressed
        if (TestData.runningWithBalancer) {
            allowedErrorCodes = allowedErrorCodes.concat(interruptedQueryErrors);
            // On slow builds with the balancer enabled, it is possible for the router to exhaust
            // all refresh attempts without converging, causing the StaleConfig error to be returned
            // to the client.
            allowedErrorCodes.push(ErrorCodes.StaleConfig);
        }

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
        jsTestLog(`Running convertToCapped: coll=${this.outputCollName}`);
        assert.commandFailedWithCode(
            db.runCommand({convertToCapped: this.outputCollName, size: 100000}), [
                ErrorCodes.MovePrimaryInProgress,
                ErrorCodes.NamespaceNotFound,
                ErrorCodes.NamespaceCannotBeSharded,
                // Can't convert a timeseries collection to a capped collection
                ErrorCodes.CommandNotSupportedOnView,
            ]);
    };

    /**
     * Same implementation as parent shardCollection, but allow ConflictingOperationInProgress error
     * which might happen as a concurrent $out creating the timeseries view.
     */
    $config.states.shardCollection = function shardCollection(db, unusedCollName) {
        if (isMongos(db) && this.tid === 0) {
            jsTestLog(`Running shardCollection: coll=${this.outputCollName} key=${this.shardKey}`);

            assert.commandWorkedOrFailedWithCode(db.adminCommand({
                shardCollection: db[this.outputCollName].getFullName(),
                key: this.shardKey,
                timeseries: {timeField: timeFieldName, metaField: metaFieldName}
            }),
                                                 [
                                                     ErrorCodes.ConflictingOperationInProgress,
                                                     // Can't shard a capped collection.
                                                     ErrorCodes.InvalidOptions
                                                 ]);
        }
    };

    /**
     * Ensures all the indexes exist. This will have no affect unless some thread has already
     * dropped an index.
     */
    $config.states.createIndexes = function createIndexes(db, unusedCollName) {
        // Create timeseries_agg_out as timeseries before running createIndex to prevent the case
        // the collection is created for the first time by the createIndex itself.
        assert.commandWorkedOrFailedWithCode(
            db.createCollection(this.outputCollName,
                                {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}),
            [ErrorCodes.NamespaceExists]);

        for (var i = 0; i < this.indexSpecs; ++i) {
            const indexSpecs = this.indexSpecs[i];
            jsTestLog(`Running createIndex: coll=${this.outputCollName} indexSpec=${indexSpecs}`);
            assert.commandWorkedOrFailedWithCode(db[this.outputCollName].createIndex(indexSpecs),
                                                 ErrorCodes.MovePrimaryInProgress);
        }
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
