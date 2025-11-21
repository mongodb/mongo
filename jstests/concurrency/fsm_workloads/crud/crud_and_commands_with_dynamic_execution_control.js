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
    $config.data.algorithms = ["fixedConcurrentTransactions", "throughputProbing"];
    $config.data.originalParams = {};

    const getServerParameterValue = (cluster, paramName) => {
        let value;
        cluster.executeOnMongodNodes((db) => {
            value = assert.commandWorked(db.adminCommand({getParameter: 1, [paramName]: 1}))[paramName];
        });
        return value;
    };

    const setServerParameterOnAllNodes = (cluster, paramName, value) => {
        cluster.executeOnMongodNodes((db) => {
            assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: value}));
        });
    };

    const setServerParameter = (db, paramName, value) => {
        jsTest.log.info(`Attempting to set ${paramName} to: ${value}`);

        assert.commandWorked(db.adminCommand({setParameter: 1, [paramName]: value}));

        jsTest.log.info(`Successfully set ${paramName} to: ${value}`);
    };

    $config.setup = function (db, collName, cluster) {
        this.originalParams = {
            algorithm: getServerParameterValue(cluster, "executionControlConcurrencyAdjustmentAlgorithm"),
            deprioritizationGate: getServerParameterValue(cluster, "executionControlDeprioritizationGate"),
            heuristicDeprioritization: getServerParameterValue(cluster, "executionControlHeuristicDeprioritization"),
            backgroundTasksDeprioritization: getServerParameterValue(
                cluster,
                "executionControlBackgroundTasksDeprioritization",
            ),
        };
    };

    $config.teardown = function (db, collName, cluster) {
        setServerParameterOnAllNodes(
            cluster,
            "executionControlConcurrencyAdjustmentAlgorithm",
            this.originalParams.algorithm,
        );
        setServerParameterOnAllNodes(
            cluster,
            "executionControlDeprioritizationGate",
            this.originalParams.deprioritizationGate,
        );
        setServerParameterOnAllNodes(
            cluster,
            "executionControlHeuristicDeprioritization",
            this.originalParams.heuristicDeprioritization,
        );
        setServerParameterOnAllNodes(
            cluster,
            "executionControlBackgroundTasksDeprioritization",
            this.originalParams.backgroundTasksDeprioritization,
        );
    };

    $config.states.setConcurrencyAlgorithm = function (db, collName) {
        const targetAlgorithm = this.algorithms[Random.randInt(this.algorithms.length)];
        setServerParameter(db, "executionControlConcurrencyAdjustmentAlgorithm", targetAlgorithm);
    };

    $config.states.setDeprioritizationGate = function (db, collName) {
        setServerParameter(db, "executionControlDeprioritizationGate", Random.randInt(2) === 1);
    };

    $config.states.setHeuristicDeprioritization = function (db, collName) {
        setServerParameter(db, "executionControlHeuristicDeprioritization", Random.randInt(2) === 1);
    };

    $config.states.setBackgroundTasksDeprioritization = function (db, collName) {
        setServerParameter(db, "executionControlBackgroundTasksDeprioritization", Random.randInt(2) === 1);
    };

    const statesProbability = {
        insertDocs: 0.14,
        updateDocs: 0.14,
        findAndModifyDocs: 0.14,
        readDocs: 0.14,
        deleteDocs: 0.14,
        dropCollection: 0.14,
        setConcurrencyAlgorithm: 0.04,
        setDeprioritizationGate: 0.04,
        setHeuristicDeprioritization: 0.04,
        setBackgroundTasksDeprioritization: 0.04,
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
        setDeprioritizationGate: statesProbability,
        setHeuristicDeprioritization: statesProbability,
        setBackgroundTasksDeprioritization: statesProbability,
    };

    return $config;
});
