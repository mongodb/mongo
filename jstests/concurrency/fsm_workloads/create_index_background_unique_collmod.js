'use strict';

/**
 * Creates multiple unique background indexes in parallel with unique index upgrade collMods.
 *
 * @tags: [creates_background_indexes]
 */

load('jstests/concurrency/fsm_libs/extend_workload.js');                      // for extendWorkload
load('jstests/concurrency/fsm_workloads/create_index_background_unique.js');  // for $config

var $config = extendWorkload($config, function($config, $super) {
    $config.data.prefix = "create_index_background_unique_collmod_";
    $config.iterations = 19;
    $config.states = Object.extend({
        collMod: function(db, collName) {
            let res = db.adminCommand({listDatabases: 1});
            assertAlways.commandWorked(res);
            let databases = res.databases;
            for (let database of databases) {
                let dbName = database["name"];
                let dbHandle = db.getSiblingDB(dbName);
                dbHandle.getCollectionInfos({$or: [{type: "collection"}, {type: {$exists: false}}]})
                    .forEach(function(collInfo) {
                        if (!collInfo.name.startsWith('system.')) {
                            try {
                                assertAlways.commandWorked(
                                    dbHandle.runCommand({collMod: collInfo.name}));
                            } catch (e) {
                                // Ignore NamespaceNotFound errors because another thread could have
                                // dropped the collection after getCollectionInfos but before
                                // running collMod.
                                if (e.code != ErrorCodes.NamespaceNotFound) {
                                    throw e;
                                }
                            }
                        }
                    });
            }
        }
    },
                                   $super.states);

    const states = Object.keys($config.states);
    let newTransitions = {};

    // Assign transitions such that each individual state can only transition to other states.
    states.forEach(function(state) {
        const otherStates = states.filter(x => x != state);
        newTransitions[state] = assignEqualProbsToTransitions(otherStates);
    });

    $config.transitions = newTransitions;

    return $config;
});
