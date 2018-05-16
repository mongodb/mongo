/**
 * Like RollbackTest, but with five nodes (a primary, two secondaries and two arbiters). This allows
 * for a controlled rollback on two nodes: one that performs writes as a primary, and one that
 * performs writes as a secondary.
 *
 * This library exposes the following 5 sequential stages of rollback:
 *
 *   1. RollbackTest starts in kSteadyStateOps: the replica set is in steady state replication.
 *          Operations applied will be replicated.
 *   2. kRollbackOps: operations applied during this phase will not be replicated and eventually be
 *          rolled back on both a primary and a secondary.
 *   3. kSyncSourceOpsBeforeRollback: apply operations on the sync source before rollback begins.
 *   4. kSyncSourceOpsDuringRollback: apply operations on the sync source after rollback has begun.
 *   5. kSteadyStateOps: (same as stage 1) with the option of waiting for the rollback to finish.
 *
 * Refer to the various "transition" functions for more information on the behavior of each stage.
 *
 * With each complete five-step rollback (a "rollback cycle"), one node rolls back writes performed
 * as a primary (the "primary"), one node rolls back writes performed as a secondary (the "rollback
 * secondary"), and one node steps up from secondary to primary (the "standby secondary"). Their
 * roles are then reassigned based on the RoleCycleMode:
 *
 *   - kCyclical:
 *          Each node rotates to a new state. Performing multiple rollback cycles in this mode
 *          allows each node to experience rollbacks in different states. Three complete rollback
 *          cycles returns each node back to its original role.
 *   - kFixedRollbackSecondary:
 *          The primary and standby secondary swap back and forth, taking turns undergoing rollback
 *          as primaries. The rollback secondary stays fixed and undergoes rollback as a secondary
 *          every time.
 *   - kRandom:
 *          Roles are assigned at random.
 *
 * The default role cycle mode is kCyclical, though it is not guaranteed to be respected in the face
 * of restarts.
 */

"use strict";

load("jstests/hooks/validate_collections.js");
load("jstests/replsets/libs/two_phase_drops.js");
load("jstests/replsets/rslib.js");

Random.setRandomSeed();

/**
 * This fixture allows the user to optionally pass in a custom ReplSetTest to be used for the test.
 * The underlying replica set must meet the following requirements:
 *
 *      1. It must have exactly five nodes: a primary, two secondaries and two arbiters.
 *      2. It must be running with mongobridge.
 *
 * If the caller does not provide their own replica set, a five-node replica set will be initialized
 * instead, with all nodes running the latest version.
 *
 * @param {string} [optional] name the name of the test being run
 * @param {Object} [optional] replSet the ReplSetTest instance to adopt
 */
function RollbackTestDeluxe(name = "FiveNodeDoubleRollbackTest", replSet) {
    const State = {
        kStopped: "kStopped",
        kRollbackOps: "kRollbackOps",
        kSyncSourceOpsBeforeRollback: "kSyncSourceOpsBeforeRollback",
        kSyncSourceOpsDuringRollback: "kSyncSourceOpsDuringRollback",
        kSteadyStateOps: "kSteadyStateOps",
    };

    const AcceptableTransitions = {
        [State.kStopped]: [],
        [State.kRollbackOps]: [State.kSyncSourceOpsBeforeRollback],
        [State.kSyncSourceOpsBeforeRollback]: [State.kSyncSourceOpsDuringRollback],
        [State.kSyncSourceOpsDuringRollback]: [State.kSteadyStateOps],
        [State.kSteadyStateOps]: [State.kStopped, State.kRollbackOps],
    };

    const collectionValidator = new CollectionValidator();

    const kEnsureRollbackDBName = "EnsureThereIsAtLeastOneOperationToRollBack";
    const kEnsureRollbackDoc = {thisDocument: "is inserted to ensure rollback is not skipped"};
    const kEnsureWritesDoc = {
        thisDocument: "is inserted so each node does at least one write before failover",
    };

    const SIGKILL = 9;
    const SIGTERM = 15;

    let rst;
    let curPrimary;
    let rollbackSecondary;
    let standbySecondary;
    let arbiters;

    let curState = State.kSteadyStateOps;
    let curRoleCycleMode = RollbackTestDeluxe.RoleCycleMode.kCyclical;
    let lastRollbackSecondaryRBID;
    let lastStandbySecondaryRBID;

    // Make sure we have a replica set up and running.
    replSet = (replSet === undefined) ? performStandardSetup() : replSet;
    validateAndUseSetup(replSet);

    /**
     * Returns the current primary of the replica set.
     */
    this.getPrimary = function() {
        return curPrimary;
    };

    /**
     * Returns an array containing connections to the arbiters, in no particular order.
     */
    this.getArbiters = function() {
        return arbiters;
    };

    /**
     * Returns an array containing connections to the data-bearing secondaries, in no particular
     * order.
     */
    this.getSecondaries = function() {
        return [rollbackSecondary, standbySecondary];
    };

    /**
     * Returns the node that is currently a secondary and will undergo rollback as a secondary.
     */
    this.getRollbackSecondary = function() {
        return rollbackSecondary;
    };

    /**
     * Returns the node that is currently a secondary but is next in line for being promoted to
     * primary.
     */
    this.getStandbySecondary = function() {
        return standbySecondary;
    };

    /**
     * Return an instance of ReplSetTest initialized with a standard five-node replica set running
     * with the latest version.
     */
    function performStandardSetup() {
        let nodeOptions = {};
        if (TestData.logComponentVerbosity) {
            nodeOptions["setParameter"] = {
                "logComponentVerbosity": tojsononeline(TestData.logComponentVerbosity)
            };
        }

        let replSet = new ReplSetTest({name, nodes: 5, useBridge: true, nodeOptions: nodeOptions});
        replSet.startSet();

        const nodes = replSet.nodeList();
        replSet.initiate({
            _id: name,
            members: [
                {_id: 0, host: nodes[0]},
                {_id: 1, host: nodes[1]},
                {_id: 2, host: nodes[2]},
                {_id: 3, host: nodes[3], arbiterOnly: true},
                {_id: 4, host: nodes[4], arbiterOnly: true},
            ]
        });
        return replSet;
    }

    /**
     * Validate and use the provided ReplSetTest instance 'replSet'.
     */
    function validateAndUseSetup(replSet) {
        assert(replSet instanceof ReplSetTest,
               `Must provide an instance of ReplSetTest. Have: ${tojson(replSet)}`);

        assert.eq(true, replSet.usesBridge(), "Must set up ReplSetTest with mongobridge enabled.");
        assert.eq(5, replSet.nodes.length, "Replica set must contain exactly five nodes.");

        // Make sure we have a primary.
        curPrimary = replSet.getPrimary();

        // Extract the other nodes and wait for them to be ready.
        arbiters = replSet.getArbiters();
        arbiters.forEach(arbiter => waitForState(arbiter, ReplSetTest.State.ARBITER));
        let secondaries = replSet.getSecondaries().filter(node => !arbiters.includes(node));
        secondaries.forEach(secondary => waitForState(secondary, ReplSetTest.State.SECONDARY));

        // Arbitrarily assign which secondary will roll back and which secondary will eventually
        // become primary.
        [rollbackSecondary, standbySecondary] = secondaries;
        rst = replSet;

        let sb = [];
        sb.push("Starting rollback test with the following roles:");
        sb.push(currentRolesToString());
        log(sb.join("\n"), true);
    }

    function checkDataConsistency() {
        assert.eq(curState,
                  State.kSteadyStateOps,
                  "Not in kSteadyStateOps state; cannot check data consistency");

        // Wait for collection drops to complete so that we don't get spurious failures during
        // consistency checks.
        rst.nodes.forEach(TwoPhaseDropCollectionTest.waitForAllCollectionDropsToComplete);

        const name = rst.name;
        // Check collection counts except when unclean shutdowns are allowed, as such a shutdown is
        // not guaranteed to preserve accurate collection counts. This count check must occur before
        // collection validation as the validate command will fix incorrect counts.
        if (!TestData.allowUncleanShutdowns || !TestData.rollbackShutdowns) {
            rst.checkCollectionCounts(name);
        }
        rst.checkOplogs(name);
        rst.checkReplicatedDataHashes(name);
        collectionValidator.validateNodes(rst.nodeList());
    }

    function log(msg, important = false) {
        if (important) {
            jsTestLog(`[${name}] ${msg}`);
        } else {
            print(`[${name}] ${msg}`);
        }
    }

    /**
     * Returns whether the cluster can transition from the current State to 'newState'.
     */
    function transitionIfAllowed(newState) {
        if (AcceptableTransitions[curState].includes(newState)) {
            log(`Transitioning to: "${newState}"`, true);
            curState = newState;
        } else {
            // Transitioning to a disallowed State is likely a bug in the code, so we throw an
            // error here instead of silently failing.
            throw new Error(`Can't transition to State "${newState}" from State "${curState}"`);
        }
    }

    /**
     * Returns a string representation of the roles of each of the data-bearing nodes, one per line.
     * Each line is prefixed by 'prefix', whose default value is the tab character.
     */
    function currentRolesToString(prefix = "\t") {
        let sb = [];
        sb.push(`${prefix}primary: ${curPrimary.host}`);
        sb.push(`${prefix}rollback secondary: ${rollbackSecondary.host}`);
        sb.push(`${prefix}standby secondary: ${standbySecondary.host}`);
        return sb.join("\n");
    }

    /**
     * Returns a document that describes the status of the nodes that will be rolled back. Intended
     * for use when inserting documents to aid debugging.
     */
    function currentStatusAsDocument() {
        return {
            shellWallClockTime: new Date().toISOString(),
            currentPrimary: curPrimary.host,
            currentRollbackSecondary: rollbackSecondary.host,
        };
    }

    /**
     * Assigns roles to nodes based on the current role cycle mode when the standby secondary
     * assumes the role of primary. This function cannot be used if any other node is the primary
     * (for example, when a restart occurs or when a stepdown is forced).
     */
    function assignRoles() {
        let sb = [];
        sb.push(`Assigning new roles to nodes. Mode: RoleCycleMode.${curRoleCycleMode}`);
        sb.push("Old roles:");
        sb.push(currentRolesToString());

        switch (curRoleCycleMode) {
            case RollbackTestDeluxe.RoleCycleMode.kCyclical:
                [rollbackSecondary, standbySecondary, curPrimary] =
                    [curPrimary, rollbackSecondary, standbySecondary];
                break;
            case RollbackTestDeluxe.RoleCycleMode.kFixedRollbackSecondary:
                [standbySecondary, curPrimary] = [curPrimary, standbySecondary];
                break;
            case RollbackTestDeluxe.RoleCycleMode.kRandom:
                let oldStandbySecondary = standbySecondary;
                [standbySecondary, rollbackSecondary] =
                    Array.shuffle([curPrimary, rollbackSecondary]);
                curPrimary = oldStandbySecondary;
                break;
            default:
                throw new Error(`Unknown role cycle mode ${curRoleCycleMode}`);
        }
        sb.push("New roles:");
        sb.push(currentRolesToString());
        log(sb.join("\n"), true);
    }

    /**
     * Set the method of determining roles for nodes.
     */
    this.setRoleCycleMode = function(newMode) {
        if (!RollbackTestDeluxe.RoleCycleMode.hasOwnProperty(newMode)) {
            throw new Error(`Unknown role cycle mode ${newMode}`);
        }

        log(`Changing role cycle mode from ${curRoleCycleMode} to ${newMode}`, true);
        curRoleCycleMode = newMode;
    };

    /**
     * Transition from a rollback state to a steady state. Operations applied in this phase will
     * be replicated to all nodes and should not be rolled back.
     */
    this.transitionToSteadyStateOperations = function() {
        // Ensure all secondaries are connected. They may already have been connected from a
        // previous stage.
        log(`Ensuring the rollback secondary ${rollbackSecondary.host} ` +
            `is connected to the other nodes`);
        rollbackSecondary.reconnect(arbiters.concat([curPrimary, standbySecondary]));

        log(`Ensuring the standby secondary ${standbySecondary.host} ` +
            `is connected to the other nodes`);
        standbySecondary.reconnect(arbiters.concat([curPrimary]));

        // Used to wait for the rollback to complete on 'node'.
        function awaitRollback(node, lastRBID) {
            log(`Waiting for rollback to complete on ${node.host}`, true);
            assert.soon(() => {
                let res = assert.adminCommandWorkedAllowingNetworkError(node, "replSetGetRBID");
                if (!res) {
                    return false;
                }

                // Fail early if the rbid is greater than lastRBID+1.
                let rbid = res.rbid;
                assert.lte(rbid,
                           lastRBID + 1,
                           `RBID is too large. current RBID: ${rbid}, last RBID: ${lastRBID}`);
                return rbid === lastRBID + 1;
            }, `Timed out waiting for RBID to increment on ${node.host}`);
        }

        // Wait for rollback to complete on both secondaries, except if shutdowns are allowed. If
        // the primary were shut down before a secondary entered rollback using it as a sync source,
        // then a secondary might get elected and not actually roll back.
        if (!TestData.rollbackShutdowns) {
            awaitRollback(rollbackSecondary, lastRollbackSecondaryRBID);
            awaitRollback(standbySecondary, lastStandbySecondaryRBID);
        } else {
            log(`Skipping RBID check on ${rollbackSecondary.host} and ${standbySecondary.host} ` +
                `because shutdowns may prevent a rollback`);
        }

        rst.awaitSecondaryNodes();
        rst.awaitReplication();

        log("Rollback and awaitReplication completed", true);

        // We call transition to steady state ops after awaiting replication has finished,
        // otherwise it could be confusing to see operations being replicated when we're already
        // in rollback complete state.
        transitionIfAllowed(State.kSteadyStateOps);

        // After the previous rollback (if any) has completed and await replication has finished,
        // the replica set should be in a consistent and "fresh" state. We now prepare for the next
        // rollback.
        checkDataConsistency();

        return curPrimary;
    };

    /**
     * Transition to the first stage of rollback testing. Writes performed on the replica set will
     * eventually be rolled back.
     *
     * In this state, the standby secondary is isolated from the rest of the replica set.
     *      P  | SS
     *      RS |
     *      A  |
     *      A  |
     */
    this.transitionToRollbackOperations = function() {
        // Ensure previous operations are replicated.
        rst.awaitSecondaryNodes();
        rst.awaitReplication();

        transitionIfAllowed(State.kRollbackOps);

        // Disconnect one secondary to wait on standby.
        log(`Isolating secondary ${standbySecondary.host} from the rest of the replica set; ` +
            `it will eventually become primary`);
        standbySecondary.disconnect(arbiters.concat([curPrimary, rollbackSecondary]));

        return curPrimary;
    };

    /**
     * Force a stepdown while in the kRollbackOps state, swapping the primary and rollback
     * secondary. This allows for each node to accept writes as both primary and secondary, which
     * will then be rolled back when transitioning to kSyncSourceOpsBeforeRollback.
     *
     * Note that this function does not change the state of the test (that is, it remains in
     * kRollbackOps). Furthermore, though the identity of the primary and rollback secondary have
     * changed, the logical topology of the replica set remains the same.
     *      P  | SS
     *      RS |
     *      A  |
     *      A  |
     */
    this.forceStepdownDuringRollbackOps = function() {
        assert.eq(
            curState,
            State.kRollbackOps,
            "forceStepdownDuringRollbackOps() can only be called while in state kRollbackOps");

        // Insert one document to ensure that each of the data-bearing nodes perform a write while
        // in their current replica set member state.
        let ensureWritesDoc = Object.extend(kEnsureWritesDoc, currentStatusAsDocument());
        assert.commandWorked(curPrimary.getDB(kEnsureRollbackDBName)
                                 .ensureWrites.insert(ensureWritesDoc, {writeConcern: {w: 2}}));

        let sb = [];
        sb.push("Old roles before forced failover:");
        sb.push(currentRolesToString());

        log(`Forcing the primary ${curPrimary.host} to step down`, true);
        assert.adminCommandWorkedAllowingNetworkError(curPrimary,
                                                      {replSetStepDown: 1, force: true});
        waitForState(curPrimary, ReplSetTest.State.SECONDARY);

        log(`Waiting for the rollback secondary ${rollbackSecondary.host} to be elected`);
        assert.soonNoExcept(() => rollbackSecondary.adminCommand("replSetStepUp").ok);

        // Ensure that the new primary is the rollback secondary.
        const newPrimary = rst.getPrimary();
        assert.eq(newPrimary,
                  rollbackSecondary,
                  `Expected the new primary to be ${rollbackSecondary.host}, ` +
                      `but ${newPrimary.host} was elected instead`);
        log(`Elected the rollback secondary ${newPrimary.host} as the new primary`);

        // Manually update the topology.
        [rollbackSecondary, curPrimary] = [curPrimary, newPrimary];

        sb.push("New roles after forced failover:");
        sb.push(currentRolesToString());
        log(sb.join("\n"), true);

        return curPrimary;
    };

    /**
     * Transition to the second stage of rollback testing, where we isolate the old primary and the
     * rollback secondary from the rest of the replica set. The arbiters are reconnected to the
     * secondary on standby to elect it as the new primary.
     *
     * In this state, operations can be performed on the new primary so that optimes diverge and
     * previous operations on the old primary and rollback secondary will be rolled back.
     *      P  | SS
     *      RS | A
     *         | A
     */
    this.transitionToSyncSourceOperationsBeforeRollback = function() {
        transitionIfAllowed(State.kSyncSourceOpsBeforeRollback);

        // Insert one document to ensure rollback will not be skipped.
        let ensureRollbackDoc = Object.extend(kEnsureRollbackDoc, currentStatusAsDocument());
        assert.commandWorked(curPrimary.getDB(kEnsureRollbackDBName)
                                 .ensureRollback.insert(ensureRollbackDoc, {writeConcern: {w: 2}}));

        log(`Isolating the rollback secondary ${rollbackSecondary.host} from the arbiters`);
        rollbackSecondary.disconnect(arbiters);

        log(`Isolating the primary ${curPrimary.host} from the arbiters so it will step down`);
        curPrimary.disconnect(arbiters);

        log(`Waiting for the primary ${curPrimary.host} to step down`);
        assert.adminCommandWorkedAllowingNetworkError(curPrimary,
                                                      {replSetStepDown: 1, force: true});

        waitForState(curPrimary, ReplSetTest.State.SECONDARY);

        log(`Reconnecting the standby secondary ${standbySecondary.host} to the arbiters ` +
            `so that it can be elected`);
        standbySecondary.reconnect(arbiters);

        log(`Waiting for the new primary ${standbySecondary.host} to be elected`);
        assert.soonNoExcept(() => standbySecondary.adminCommand("replSetStepUp").ok);

        const newPrimary = rst.getPrimary();

        // Ensure that the new primary is the standby secondary.
        assert.eq(newPrimary,
                  standbySecondary,
                  `Expected the new primary to be ${standbySecondary.host}, ` +
                      `but ${newPrimary.host} was elected instead`);
        log(`Elected the standby secondary ${newPrimary.host} as the new primary`);

        // Update the topology so that each node gets a new role the next time we enter rollback.
        assignRoles(newPrimary);

        // Keep track of the last RBID on each node.
        lastRollbackSecondaryRBID =
            assert.commandWorked(rollbackSecondary.adminCommand("replSetGetRBID")).rbid;
        lastStandbySecondaryRBID =
            assert.commandWorked(standbySecondary.adminCommand("replSetGetRBID")).rbid;

        return curPrimary;
    };

    /**
     * Transition to the third stage of rollback testing, where we reconnect the rollback node so
     * it will start rolling back.
     *
     * Note that there is no guarantee that operations performed on the sync source while in this
     * state will actually occur *during* the rollback process. They may happen before the rollback
     * is finished or after the rollback is done. We provide this state, though, as an attempt to
     * provide a way to test this behavior, even if it's non-deterministic.
     */
    this.transitionToSyncSourceOperationsDuringRollback = function() {
        transitionIfAllowed(State.kSyncSourceOpsDuringRollback);

        log(`Reconnecting former primary ${rollbackSecondary.host} so it will enter rollback`);
        rollbackSecondary.reconnect(arbiters.concat([curPrimary]));

        log(`Reconnecting former rollback secondary ${standbySecondary.host} so ` +
            `it will enter rollback`);
        standbySecondary.reconnect(arbiters.concat([curPrimary]));
        return curPrimary;
    };

    /**
     * Transitions to the stop state, stopping the replica set.
     */
    this.stop = function() {
        checkDataConsistency();
        transitionIfAllowed(State.kStopped);
        return rst.stopSet();
    };

    /**
     * Sends the signal 'signal' to the node with id 'nodeId'.
     */
    this.restartNode = function(nodeId, signal) {
        assert(signal === SIGKILL || signal === SIGTERM, `Received unknown signal: ${signal}`);
        assert.gte(nodeId, 0, "Node id number cannot be negative");

        const hostName = rst.nodes[nodeId].host;

        if (!TestData.rollbackShutdowns) {
            log(`Not restarting node ${hostName} because 'rollbackShutdowns' was not specified.`);
            return;
        }

        if (nodeId >= rst.nodes.length) {
            log(`Not restarting node ${nodeId} because this replica set is too small.`);
            return;
        }

        if (!TestData.allowUncleanShutdowns && signal !== SIGTERM) {
            log(`Sending node ${hostName} signal ${SIGTERM}` +
                ` instead of ${signal} because 'allowUncleanShutdowns' was not specified.`);
            signal = SIGTERM;
        }

        let opts = {};
        if (signal === SIGKILL) {
            opts = {allowedExitCode: MongoRunner.EXIT_SIGKILL};
        }

        log(`Stopping node ${hostName} with signal ${signal}`);
        rst.stop(nodeId, signal, opts);

        log(`Restarting node ${hostName}`);
        const restart = true;
        rst.start(nodeId, {}, restart);

        let newPrimary = rst.getPrimary();
        if (newPrimary === rollbackSecondary) {
            assert(curState === State.kSteadyStateOps || curState === State.kRollbackOps,
                   `Expected to be in state kSteadyStateOps or kRollbackOps, ` +
                       `but instead the state is ${curState}`);

            // This restart is special so we don't follow the usual role assignment path. Log a
            // message explaining the role change.
            let sb = [];
            sb.push(`Assigning new roles to nodes. Reason: ${hostName} was restarted`);
            sb.push("Old roles:");
            sb.push(currentRolesToString());

            // Swap the primary and rollback secondary.
            [rollbackSecondary, curPrimary] = [curPrimary, newPrimary];

            sb.push("New roles:");
            sb.push(currentRolesToString());
            log(sb.join("\n"), true);
        } else if (newPrimary === standbySecondary) {
            assert.eq(curState, State.kSteadyStateOps);

            // Follow the usual transition rules.
            assignRoles(newPrimary);
        } else {
            // The primary must have stayed the same. The roles of each member of the set do not
            // change.
            assert.eq(curPrimary, newPrimary);
        }
    };
}

RollbackTestDeluxe.RoleCycleMode = {
    kCyclical: "kCyclical",
    kFixedRollbackSecondary: "kFixedRollbackSecondary",
    kRandom: "kRandom",
};
