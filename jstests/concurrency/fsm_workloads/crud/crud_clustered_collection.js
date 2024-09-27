/**
 * Perform CRUD operations in parallel on a clustered collection. Disallows dropping the collection
 * to prevent implicit creation of a non-clustered collection.
 *
 * @tags: [
 *  requires_fcv_53
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/crud_and_commands.js";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    // Exclude dropCollection to prevent implicit collection creation of a non-clustered
    // collection.
    const newStates = $super.states;
    delete newStates.dropCollection;

    $config.states = Object.extend({
        createIndex: function createIndex(db, collName) {
            db[collName].createIndex({value: 1});
        },
        dropIndex: function dropIndex(db, collName) {
            db[collName].dropIndex({value: 1});
        },
        listCollections: function listCollections(db, collName) {
            const listColls =
                assert.commandWorked(db.runCommand({listCollections: 1, filter: {name: collName}}));
            const listCollsOptions = listColls.cursor.firstBatch[0].options;
            assert(listCollsOptions.clusteredIndex);
        },
    },
                                   newStates);

    $config.setup = function(db, coll, cluster) {
        // As the default collection created by runner.js won't be clustered we need to recreate it.
        db[coll].drop();

        assert.commandWorked(
            db.runCommand({create: coll, clusteredIndex: {key: {_id: 1}, unique: true}}));
        for (let i = 0; i < this.numIds; i++) {
            const res = db[coll].insert({_id: i, value: this.docValue, num: 1});
            assert.commandWorked(res);
            assert.eq(1, res.nInserted);
        }

        if (cluster.isSharded()) {
            cluster.shardCollection(db[coll], {_id: 1}, true);
        }
    };

    // Exclude dropCollection to prevent implicit collection creation of a non-clustered
    // collection.
    const newTransitions = Object.extend({}, $super.transitions);
    delete newTransitions.dropCollection;

    let defaultTransitionWeights = {};
    Object.keys(newTransitions).forEach(function(stateName) {
        if (newTransitions[stateName].dropCollection) {
            delete newTransitions[stateName]["dropCollection"];
        }

        newTransitions[stateName]["createIndex"] = 0.10;
        newTransitions[stateName]["dropIndex"] = 0.10;
        newTransitions[stateName]["listCollections"] = 0.10;

        if (stateName !== "init") {
            defaultTransitionWeights = newTransitions[stateName];
        }
    });
    newTransitions["createIndex"] = defaultTransitionWeights;
    newTransitions["dropIndex"] = defaultTransitionWeights;
    newTransitions["listCollections"] = defaultTransitionWeights;

    $config.transitions = newTransitions;

    return $config;
});
