/**
 * Perform CRUD operations, some of which may implicitly create collections. Also perform index
 * creations which may implicitly create collections. Performs these in parallel with collection-
 * dropping operations.
 */
import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/crud_and_commands.js";

// TODO(SERVER-46971) combine with crud_and_commands.js and remove `local` readConcern.
TestData.defaultTransactionReadConcernLevel = "local";

export const $config = extendWorkload($baseConfig, function($config, $super) {
    const origStates = Object.keys($config.states);
    $config.states = Object.extend({
        createIndex: function createIndex(db, collName) {
            db[collName].createIndex({value: 1});
        },
        createIdIndex: function createIdIndex(db, collName) {
            let created = false;
            while (!created) {
                try {
                    assert.commandWorked(db[collName].createIndex({_id: 1}));
                    created = true;
                } catch (e) {
                    if (e.code != ErrorCodes.ConflictingOperationInProgress) {
                        // unexpected error, rethrow
                        throw e;
                    }
                    // createIndex concurrently with dropCollection can throw a conflict.
                    // fall through to retry
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
