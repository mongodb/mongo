/**
 * Repeatedly creates a collection and a view with the same namespace. Validates that we never
 * manage to have both a Collection and View created on the same namespace at the same time.
 *
 * @tags: [catches_command_failures, antithesis_incompatible]
 */

import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export const $config = (function () {
    const prefix = "create_collection_and_view";

    const allowedErrorCodes = [ErrorCodes.NamespaceExists];

    // We'll use a single unique collection for all operations executed by this test. The
    // convention is that each FSM workload is given a unique 'collName' as input to increase
    // separation between different tests running in parallel. Otherwise we could just use a fixed
    // name, but we want to play nice.
    const getCollectionName = (collName) => {
        return prefix + "_" + collName;
    };

    const getBaseCollectionName = (collName) => {
        return prefix + "_" + collName + "_base";
    };

    const setup = (db, collName) => {
        assert.commandWorked(db.createCollection(getBaseCollectionName(collName)));
    };

    const teardown = (db, collName) => {
        db.getCollection(getCollectionName(collName)).drop();
        db.getCollection(getBaseCollectionName(collName)).drop();
    };

    const states = {
        init: (db, collName) => {
            if (isMongos(db)) {
                // In a sharded collection, we may sometimes get a NamespaceNotFound error, as we
                // attempt to to do some additional validation on the creation options after we get
                // back the NamespaceExists error, and the namespace may have been dropped in the
                // meantime.
                allowedErrorCodes.push(ErrorCodes.NamespaceNotFound);
                // In rare cases, it's possible the max number of retries is hit and eventually the
                // router returns a StaleConfig error.
                allowedErrorCodes.push(ErrorCodes.StaleConfig);
                // Due to the above, in very rare cases, it is possible that the router has
                // exhausted all but the last retry due to StaleConfig, and a suite which runs
                // movePrimary causes a StaleDbVersion, which will surface to the client after the
                // last retry.
                allowedErrorCodes.push(ErrorCodes.StaleDbVersion);
            }
        },
        createView: (db, collName) => {
            assert.commandWorkedOrFailedWithCode(
                db.createCollection(getCollectionName(collName), {
                    viewOn: getBaseCollectionName(collName),
                    pipeline: [{$match: {}}],
                }),
                allowedErrorCodes,
            );
        },
        createCollection: (db, collName) => {
            assert.commandWorkedOrFailedWithCode(db.createCollection(getCollectionName(collName)), allowedErrorCodes);
        },
        verifyNoDuplicates: (db, collName) => {
            // Check how many collections/views match our namespace.
            const res = db.runCommand("listCollections", {filter: {name: getCollectionName(collName)}});
            assert.commandWorked(res);
            // We expect that we only ever find 0 or 1. If we find 2 or more, then we managed to
            // create a view and collection on the same namespace simultaneously, which is a bug.
            assert.lte(res.cursor.firstBatch.length, 1);
        },
        dropNamespace: (db, collName) => {
            db.getCollection(getCollectionName(collName)).drop();
        },
    };

    const transitions = {
        init: {createCollection: 0.5, createView: 0.5},

        // Always verify after creation because there's no point in creating an invalid situation
        // (view and collection simultaneously on the same namespace) if we don't observe it.
        createCollection: {verifyNoDuplicates: 1.0},
        createView: {verifyNoDuplicates: 1.0},

        verifyNoDuplicates: {dropNamespace: 1.0},
        dropNamespace: {createCollection: 0.5, createView: 0.5},
    };

    return {
        threadCount: 10,
        iterations: 150,
        setup,
        states,
        teardown,
        transitions,
    };
})();
