'use strict';

/**
 * Perform CRUD operations, some of which may implicitly create collections. Also perform index
 * creations which may implicitly create collections. Performs these in parallel with collection-
 * dropping operations.
 */
load('jstests/concurrency/fsm_libs/extend_workload.js');         // for extendWorkload
load('jstests/concurrency/fsm_workloads/CRUD_and_commands.js');  // for $config

// TODO(SERVER-46971) combine with CRUD_and_commands.js and remove `local` readConcern.
TestData.defaultTransactionReadConcernLevel = "local";

var $config = extendWorkload($config, function($config, $super) {
    const origStates = Object.keys($config.states);
    $config.states = Object.extend({
        createIndex: function createIndex(db, collName) {
            db[collName].createIndex({value: 1});
        },
        createIdIndex: function createIdIndex(db, collName) {
            try {
                assertWhenOwnColl.commandWorked(db[collName].createIndex({_id: 1}));
            } catch (e) {
                if (e.code == ErrorCodes.ConflictingOperationInProgress) {
                    // createIndex concurrently with dropCollection can throw.
                    if (TestData.runInsideTransaction) {
                        e["errorLabels"] = ["TransientTransactionError"];
                        throw e;
                    }
                }
            }
        },

        dropIndex: function dropIndex(db, collName) {
            db[collName].dropIndex({value: 1});
        }
    },
                                   $super.states);

    let newTransitions = Object.extend({}, $super.transitions);
    let exampleState = {};
    origStates.forEach(function(state) {
        newTransitions[state]["createIndex"] = 0.10;
        newTransitions[state]["createIdIndex"] = 0.10;
        newTransitions[state]["dropIndex"] = 0.10;
        if (state !== "init" && state !== "dropCollection") {
            exampleState = $config.transitions[state];
        }
    });

    newTransitions["createIndex"] = exampleState;
    newTransitions["createIdIndex"] = exampleState;
    newTransitions["dropIndex"] = exampleState;

    $config.transitions = newTransitions;
    return $config;
});
