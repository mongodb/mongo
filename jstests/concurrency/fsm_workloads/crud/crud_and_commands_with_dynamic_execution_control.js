/**
 * Performs a series of CRUD operations and commands, verifying that guarantees are not broken even
 * as the execution control algorithm is changed dynamically at runtime.
 *
 * @tags: [
 *   requires_replication,
 *   multiversion_incompatible,
 *  ]
 */

import {extendWorkload} from "jstests/concurrency/fsm_libs/extend_workload.js";
import {$config as $baseConfig} from "jstests/concurrency/fsm_workloads/crud/crud_and_commands.js";

export const $config = extendWorkload($baseConfig, function ($config, $super) {
    $config.data.algorithms = [
        "fixedConcurrentTransactions",
        "fixedConcurrentTransactionsWithPrioritization",
        "throughputProbing",
    ];

    $config.data.originalAlgorithmValue = "";

    $config.setup = function (db, collName, cluster) {
        cluster.executeOnMongodNodes(function (db) {
            $config.data.originalAlgorithmValue = assert.commandWorked(
                db.adminCommand({
                    getParameter: 1,
                    executionControlConcurrencyAdjustmentAlgorithm: 1,
                }),
            ).executionControlConcurrencyAdjustmentAlgorithm;
        });
    };

    $config.teardown = function (db, collName, cluster) {
        cluster.executeOnMongodNodes(function (db) {
            assert.commandWorked(
                db.adminCommand({
                    setParameter: 1,
                    executionControlConcurrencyAdjustmentAlgorithm: $config.data.originalAlgorithmValue,
                }),
            );
        });
    };

    $config.states.setConcurrencyAlgorithm = function (db, collName) {
        const targetAlgorithm = $config.data.algorithms[Random.randInt($config.data.algorithms.length)];

        jsTest.log.info(`Attempting to set executionControlConcurrencyAdjustmentAlgorithm to: ${targetAlgorithm}`);

        assert.commandWorked(
            db.adminCommand({setParameter: 1, executionControlConcurrencyAdjustmentAlgorithm: targetAlgorithm}),
        );

        jsTest.log.info(`Successfully set executionControlConcurrencyAdjustmentAlgorithm to: ${targetAlgorithm}`);
    };

    const statesProbability = {
        insertDocs: 0.16,
        updateDocs: 0.16,
        findAndModifyDocs: 0.16,
        readDocs: 0.16,
        deleteDocs: 0.16,
        dropCollection: 0.16,
        setConcurrencyAlgorithm: 0.04,
    };
    $config.transitions = {
        init: statesProbability,
        insertDocs: statesProbability,
        updateDocs: statesProbability,
        findAndModifyDocs: statesProbability,
        readDocs: statesProbability,
        deleteDocs: statesProbability,
        dropCollection: statesProbability,
        setConcurrencyAlgorithm: statesProbability,
    };

    return $config;
});
