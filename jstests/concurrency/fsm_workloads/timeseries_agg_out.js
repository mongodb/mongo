'use strict';
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
 *   featureFlagAggOutTimeseries
 * ]
 */
load('jstests/concurrency/fsm_workloads/agg_out.js');  // for $super state functions

var $config = extendWorkload($config, function($config, $super) {
    const timeFieldName = 'time';
    const metaFieldName = 'tag';
    const numDocs = 100;
    $config.data.outputCollName = 'timeseries_agg_out';
    /**
     * Runs an aggregate with a $out with time-series into '$config.data.outputCollName'.
     */
    $config.states.query = function query(db, collName) {
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
            ErrorCodes.CommandFailed,     // indexes of target collection changed during processing.
            ErrorCodes.IllegalOperation,  // $out is not supported to an existing *sharded* output
            // collection.
            17152,  // namespace is capped so it can't be used for $out.
            28769,  // $out collection cannot be sharded.
        ];
        assertWhenOwnDB.commandWorkedOrFailedWithCode(res, allowedErrorCodes);
        if (res.ok) {
            const cursor = new DBCommandCursor(db, res);
            assertAlways.eq(0, cursor.itcount());  // No matter how many documents were in the
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

        assertWhenOwnDB.commandWorkedOrFailedWithCode(
            db.runCommand({collMod: this.outputCollName, expireAfterSeconds: expireAfterSeconds}),
            ErrorCodes.ConflictingOperationInProgress);
    };

    /**
     * 'convertToCapped' should always fail with a 'CommandNotSupportedOnView' error.
     */
    $config.states.convertToCapped = function convertToCapped(db, unusedCollName) {
        if (isMongos(db)) {
            return;  // convertToCapped can't be run against a mongos.
        }
        assertWhenOwnDB.commandFailedWithCode(
            db.runCommand({convertToCapped: this.outputCollName, size: 100000}),
            ErrorCodes.CommandNotSupportedOnView);
    };

    /**
     * If being run against a mongos, shards '$config.data.outputCollName'. This is never undone,
     * and all subsequent $out's to this collection should fail.
     */
    $config.states.shardCollection = function shardCollection(db, unusedCollName) {
        // TODO SERVER-76626 change this from a no-op.
        return;
    };

    $config.teardown = function teardown(db) {
        const collNames = db.getCollectionNames();
        // Ensure that a temporary collection is not left behind.
        assertAlways.eq(db.getCollectionNames()
                            .filter(col => col.includes('system.buckets.tmp.agg_out'))
                            .length,
                        0);

        // Ensure that for the buckets collection there is a corresponding view.
        assertAlways(!(collNames.includes('system.buckets.timeseries_agg_out') &&
                       !collNames.includes('timeseries_agg_out')));
    };

    /**
     * Create a time-series collection and insert 100 documents.
     */
    $config.setup = function setup(db, collName, cluster) {
        db[collName].drop();
        assertWhenOwnDB.commandWorked(db.createCollection(
            collName, {timeseries: {timeField: timeFieldName, metaField: metaFieldName}}));
        const docs = [];
        for (let i = 0; i < numDocs; ++i) {
            docs.push({
                [timeFieldName]: ISODate(),
                [metaFieldName]: (this.tid * numDocs) + i,
            });
        }
        assertWhenOwnDB.commandWorked(
            db.runCommand({insert: collName, documents: docs, ordered: false}));
    };

    return $config;
});
