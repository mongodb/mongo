'use strict';

/**
 * Perform CRUD operations in parallel on a clustered collection. Disallows dropping the collection
 * to prevent implicit creation of a non-clustered collection.
 *
 * @tags: [
 *  requires_fcv_51,
 *  featureFlagClusteredIndexes
 *  ]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/CRUD_and_commands.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
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

        cluster.executeOnMongodNodes(function(nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(
                {configureFailPoint: 'clusterAllCollectionsByDefault', mode: 'alwaysOn'}));
        });

        $super.setup.apply(this, [db, coll, cluster]);

        if (cluster.isSharded()) {
            cluster.shardCollection(db[coll], {_id: 1}, true);
        }
    };

    $config.teardown = function(db, collName, cluster) {
        $super.teardown.apply(this, [db, collName, cluster]);

        cluster.executeOnMongodNodes(function(nodeAdminDB) {
            assert.commandWorked(nodeAdminDB.runCommand(
                {configureFailPoint: 'clusterAllCollectionsByDefault', mode: 'off'}));
        });
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
