/**
 * create_capped_collection_visibility_snapshot.js
 *
 * Repeatedly creates a capped collection, while concurrent readers try to establish a capped
 * visibility snapshot.
 *
 * @tags: [
 *  requires_capped,
 *  # This test works on a capped collection, which do not support sharding.
 *  assumes_unsharded_collection,
 * ]
 */
import {assertAlways, interruptedQueryErrors} from "jstests/concurrency/fsm_libs/assert.js";

export const $config = (function() {
    const data = {
        // Use the workload name as a prefix for the collection name,
        // since the workload name is assumed to be unique.
        prefix: 'create_capped_collection_visibility_snapshot',
        collectionCount: 2,
    };

    const states = (function() {
        const options = {
            capped: true,
            size: 8192  // multiple of 256; larger than 4096 default
        };

        function randomCollectionName(prefix, collCount) {
            return prefix + Random.randInt(collCount);
        }

        function create(db, collName) {
            const localDb = db.getSiblingDB("local");
            const myCollName = randomCollectionName(this.prefix, this.collectionCount);
            localDb.runCommand({drop: myCollName});
            localDb.createCollection(myCollName, options);
            localDb[myCollName].insert({x: 1});
        }

        function findOne(db, collName) {
            const localDb = db.getSiblingDB("local");
            const myCollName = randomCollectionName(this.prefix, this.collectionCount);
            for (let i = 0; i < 10; ++i) {
                let res = localDb.runCommand({find: myCollName, filter: {}});
                assertAlways.commandWorked(res);
            }
        }

        function getMore(db, collName) {
            const localDb = db.getSiblingDB("local");
            const myCollName = randomCollectionName(this.prefix, this.collectionCount);
            for (let i = 0; i < 10; ++i) {
                let res = localDb.runCommand(
                    {find: myCollName, filter: {}, tailable: true, batchSize: 0});
                assertAlways.commandWorked(res);
                assertAlways.commandWorkedOrFailedWithCode(
                    localDb.runCommand({getMore: res.cursor.id, collection: myCollName}),
                    interruptedQueryErrors);
            }
        }

        return {create: create, findOne: findOne, getMore: getMore};
    })();

    let internalQueryExecYieldIterationsDefault;

    function setup(db, collName, cluster) {
        // We temporarily reduce the query yield iterations to force yield/restore on getMore.
        cluster.executeOnMongodNodes((db) => {
            const res = db.adminCommand({setParameter: 1, internalQueryExecYieldIterations: 1});
            assert.commandWorked(res);
            internalQueryExecYieldIterationsDefault = res.was;
        });
    }

    function teardown(db, collName, cluster) {
        cluster.executeOnMongodNodes((db) => {
            const res = db.adminCommand({
                setParameter: 1,
                internalQueryExecYieldIterations: internalQueryExecYieldIterationsDefault
            });
            assert.commandWorked(res);
        });
    }

    const transition = {create: 1, findOne: 4, getMore: 4};

    const transitions = {create: transition, findOne: transition, getMore: transition};

    return {
        threadCount: 20,
        // TODO(SERVER-81258): Return to `iterations: 100` after build failure is resolved
        iterations: 0,
        data: data,
        startState: 'create',
        states: states,
        transitions: transitions,
        setup: setup,
        teardown: teardown,
    };
})();
