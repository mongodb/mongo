/**
 * agg_out.js
 *
 * This test runs many concurrent aggregations using $out, writing to the same collection. While
 * this is happening, other threads may be creating or dropping indexes, changing the collection
 * options, or sharding the collection. We expect an aggregate with a $out stage to fail if another
 * client executed one of these changes between the creation of $out's temporary collection and the
 * eventual rename to the target collection.
 *
 * Unfortunately, there aren't very many assertions we can make here, so this is mostly to test that
 * the server doesn't deadlock or crash.
 *
 * @tags: [
 *   requires_capped,
 *   # Uses $out, which is non-retryable.
 *   requires_non_retryable_writes,
 *   requires_getmore,
 *   uses_getmore_outside_of_transaction,
 * ]
 */

// TODO SERVER-92452: This test fails in burn-in with the 'inMemory' engine with the 'WT_CACHE_FULL'
// error. This is a known issue and can be ignored. Remove this comment once SERVER-92452 is fixed.

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/query/agg/agg_base.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.iterations = 100;

    $config.data.outputCollName = "agg_out"; // Use the workload name as the collection name
    // because it is assumed to be unique.

    $config.data.indexSpecs = [{rand: -1, randInt: 1}, {randInt: -1}, {flag: 1}, {padding: "text"}];
    $config.data.shardKey = {_id: "hashed"};

    // We'll use document validation so that we can change the collection options in the middle of
    // an $out, to test that the $out stage will notice this and error. This validator is not very
    // interesting, and documents should always pass.
    $config.data.documentValidator = {flag: {$exists: true}};

    $config.transitions = {
        query: {
            query: 0.58,
            createIndexes: 0.1,
            dropIndex: 0.1,
            collMod: 0.1,
            untrackUnshardedCollection: 0.05,
            movePrimary: 0.05,
            // Converting the target collection to a capped collection or a sharded collection will
            // cause all subsequent aggregations to fail, so give these a low probability to make
            // sure they don't happen too early in the test.
            convertToCapped: 0.01,
            shardCollection: 0.01,
        },
        createIndexes: {query: 1},
        movePrimary: {query: 1},
        dropIndex: {query: 1},
        collMod: {query: 1},
        convertToCapped: {query: 1},
        shardCollection: {query: 1},
        untrackUnshardedCollection: {query: 1},
    };

    /**
     * Runs an aggregate with a $out into '$config.data.outputCollName'.
     */
    $config.states.query = function query(db, collName) {
        jsTestLog(`Running query: coll=${collName} out=${this.outputCollName}`);
        const res = db[collName].runCommand({
            aggregate: collName,
            pipeline: [{$match: {flag: true}}, {$out: this.outputCollName}],
            cursor: {},
        });

        const allowedErrorCodes = [
            // Indexes of target collection changed during processing.
            ErrorCodes.CommandFailed,
            // $out is not supported to an existing *sharded* output collection.
            ErrorCodes.IllegalOperation,
            // Namespace is capped so it can't be used for $out.
            17152,
            // $out collection cannot be sharded.
            ErrorCodes.NamespaceCannotBeSharded,
            // $out can't be executed while there is a move primary in progress.
            ErrorCodes.MovePrimaryInProgress,
            // (SERVER-78850) Move primary coordinator drops donor collections when it is done.
            // This invalidates non - snapshot cursors, which causes $out to fail.
            // Locally it fails with explicit collection dropped error. When doing remote reads,
            // it fails with cursor not found error.
            ErrorCodes.CursorNotFound,
            // When running in suites with random migrations $out can fail copying the indexes due
            // to a resharding operation in progress
            ErrorCodes.ReshardCollectionInProgress,
            // A cluster toplogy change (for example, a replica set step down/step up or a
            // movePrimary) will cause $out's temporary collection to be dropped part way through.
            // $out will detect this when it sees that the UUID of the temporary collection has
            // changed across inserts and throw a CollectionUUIDMismatch error.
            ErrorCodes.CollectionUUIDMismatch,
        ];
        assert.commandWorkedOrFailedWithCode(res, allowedErrorCodes);

        if (res.ok) {
            // No matter how many documents were in the original input stream, $out should never
            // return any results.
            const cursor = new DBCommandCursor(db, res);
            assert.eq(0, cursor.itcount());
        }
    };

    /**
     * Ensures all the indexes exist. This will have no affect unless some thread has already
     * dropped an index.
     */
    $config.states.createIndexes = function createIndexes(db, unusedCollName) {
        for (let i = 0; i < this.indexSpecs; ++i) {
            const indexSpecs = this.indexSpecs[i];
            jsTestLog(`Running createIndex: coll=${this.outputCollName} indexSpec=${indexSpecs}`);
            assert.commandWorkedOrFailedWithCode(
                db[this.outputCollName].createIndex(indexSpecs),
                ErrorCodes.MovePrimaryInProgress,
            );
        }
    };

    /**
     * Drops a random index from '$config.data.indexSpecs'.
     */
    $config.states.dropIndex = function dropIndex(db, unusedCollName) {
        const indexSpec = this.indexSpecs[Random.randInt(this.indexSpecs.length)];
        jsTestLog(`Running dropIndex: coll=${this.outputCollName} indexSpec=${indexSpec}`);
        db[this.outputCollName].dropIndex(indexSpec);
    };

    /**
     * Changes the document validation options for the collection.
     */
    $config.states.collMod = function collMod(db, unusedCollName) {
        if (Random.rand() < 0.5) {
            // Change the validation level.
            const validationLevels = ["off", "strict", "moderate"];
            const newValidationLevel = validationLevels[Random.randInt(validationLevels.length)];
            jsTestLog(`Running collMod: coll=${this.outputCollName} validationLevel=${newValidationLevel}`);

            assert.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: this.outputCollName, validationLevel: newValidationLevel}),
                [ErrorCodes.ConflictingOperationInProgress, ErrorCodes.MovePrimaryInProgress],
            );
        } else {
            // Change the validation action.
            const validationAction = Random.rand() > 0.5 ? "warn" : "error";
            jsTestLog(`Running collMod: coll=${this.outputCollName} validationAction=${validationAction}`);

            assert.commandWorkedOrFailedWithCode(
                db.runCommand({collMod: this.outputCollName, validationAction: validationAction}),
                [ErrorCodes.ConflictingOperationInProgress, ErrorCodes.MovePrimaryInProgress],
            );
        }
    };

    $config.states.movePrimary = function movePrimary(db, collName) {
        if (!isMongos(db)) {
            return;
        }
        const toShard = this.shards[Random.randInt(this.shards.length)];
        jsTestLog(`Running movePrimary: db=${db} to=${toShard}`);

        let expectedErrorCodes = [
            // Caused by a concurrent movePrimary operation on the same database but a
            // different destination shard.
            ErrorCodes.ConflictingOperationInProgress,
            // Due to a stepdown of the donor during the cloning phase, the movePrimary
            // operation failed. It is not automatically recovered, but any orphaned data on
            // the recipient has been deleted.
            7120202,
            // In the FSM tests, there is a chance that there are still some User
            // collections left to clone. This occurs when a MovePrimary joins an already
            // existing MovePrimary command that has purposefully triggered a failpoint.
            9046501,
        ];
        if (TestData.hasRandomShardsAddedRemoved) {
            expectedErrorCodes.push(ErrorCodes.ShardNotFound);
        }
        assert.commandWorkedOrFailedWithCode(
            db.adminCommand({movePrimary: db.getName(), to: toShard}),
            expectedErrorCodes,
        );
    };

    /*
     * Untrack the collection from the sharding catalog.
     */
    $config.states.untrackUnshardedCollection = function untrackCollection(db, collName) {
        // Note this command will behave as no-op in case the collection is not tracked.
        const namespace = `${db}.${collName}`;
        jsTestLog(`Running untrackUnshardedCollection: ns=${namespace}`);
        if (isMongos(db)) {
            assert.commandWorkedOrFailedWithCode(db.adminCommand({untrackUnshardedCollection: namespace}), [
                // Handles the case where the collection is not located on its primary
                ErrorCodes.OperationFailed,
                // Handles the case where the collection is sharded
                ErrorCodes.InvalidNamespace,
                // Handles the case where the collection/db does not exist
                ErrorCodes.NamespaceNotFound,
            ]);
        }
    };

    /**
     * Converts '$config.data.outputCollName' to a capped collection. This is never undone, and
     * all subsequent $out's to this collection should fail.
     */
    $config.states.convertToCapped = function convertToCapped(db, unusedCollName) {
        jsTestLog(`Running convertToCapped: coll=${this.outputCollName}`);
        assert.commandWorkedOrFailedWithCode(db.runCommand({convertToCapped: this.outputCollName, size: 100000}), [
            ErrorCodes.MovePrimaryInProgress,
            ErrorCodes.NamespaceNotFound,
            ErrorCodes.NamespaceCannotBeSharded,
        ]);
    };

    /**
     * If being run against a mongos, shards '$config.data.outputCollName'. This is never undone,
     * and all subsequent $out's to this collection should fail. Collection sharding is restricted
     * to a single thread as multiple concurrent invocations can result in command timeout /
     * failure.
     */
    $config.states.shardCollection = function shardCollection(db, unusedCollName) {
        if (isMongos(db) && this.tid === 0) {
            jsTestLog(`Running shardCollection: coll=${this.outputCollName} key=${this.shardKey}`);
            assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));

            try {
                assert.commandWorked(
                    db.adminCommand({shardCollection: db[this.outputCollName].getFullName(), key: this.shardKey}),
                );
            } catch (e) {
                const exceptionCode = e.code;
                if (exceptionCode) {
                    if (exceptionCode == ErrorCodes.InvalidOptions) {
                        // Expected errors:
                        // - `ErrorCodes.InvalidOptions`: Can't shard a capped collection.
                        return;
                    }
                }
                throw e;
            }
        }
    };

    /**
     * Calls the super class' setup but using our own database.
     */
    $config.setup = function setup(db, collName, cluster) {
        // Use a smaller document size, but more iterations. The smaller documents will ensure each
        // operation is faster, giving us time to do more operations and thus increasing the
        // likelihood that any two operations will be happening concurrently.
        this.docSize = 1000;
        $super.setup.apply(this, [db, collName, cluster]);

        // `shardCollection()` requires a shard key index to be in place on the output collection,
        // as we may be sharding a non-empty collection.
        assert.commandWorked(db[this.outputCollName].createIndex({_id: "hashed"}));

        if (isMongos(db)) {
            this.shards = Object.keys(cluster.getSerializedCluster().shards);
        }
    };

    return $config;
});
