/**
 * DEPRECATED (SERVER-35002): RollbackTest is deprecated. Please use RollbackTestDeluxe instead.
 *
 * Wrapper around ReplSetTest for testing rollback behavior. It allows the caller to easily
 * transition between stages of a rollback without having to manually operate on the replset.
 *
 * This library exposes the following 5 sequential stages of rollback:
 * 1. RollbackTest starts in kSteadyStateOps: the replica set is in steady state replication.
 *        Operations applied will be replicated.
 * 2. kRollbackOps: operations applied during this phase will not be replicated and eventually be
 *        rolled back.
 * 3. kSyncSourceOpsBeforeRollback: apply operations on the sync source before rollback begins.
 * 4. kSyncSourceOpsDuringRollback: apply operations on the sync source after rollback has begun.
 * 5. kSteadyStateOps: (same as stage 1) with the option of waiting for the rollback to finish.
 *
 * Please refer to the various `transition*` functions for more information on the behavior
 * of each stage.
 */

"use strict";

load("jstests/replsets/rslib.js");
load("jstests/replsets/libs/two_phase_drops.js");
load("jstests/hooks/validate_collections.js");

/**
 * DEPRECATED (SERVER-35002): RollbackTest is deprecated. Please use RollbackTestDeluxe instead.
 *
 * This fixture allows the user to optionally pass in a custom ReplSetTest
 * to be used for the test. The underlying replica set must meet the following
 * requirements:
 *      1. It must have exactly three nodes: a primary, a secondary and an arbiter.
 *      2. It must be running with mongobridge.
 *
 * If the caller does not provide their own replica set, a standard three-node
 * replset will be initialized instead, with all nodes running the latest version.
 *
 * @param {string} [optional] name the name of the test being run
 * @param {Object} [optional] replSet the ReplSetTest instance to adopt
 */
function RollbackTest(name = "RollbackTest", replSet) {
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

    const SIGKILL = 9;
    const SIGTERM = 15;
    const kNumDataBearingNodes = 2;

    let rst;
    let curPrimary;
    let curSecondary;
    let arbiter;

    let curState = State.kSteadyStateOps;
    let lastRBID;

    // Make sure we have a replica set up and running.
    replSet = (replSet === undefined) ? performStandardSetup() : replSet;
    validateAndUseSetup(replSet);

    /**
     * Validate and use the provided replica set.
     *
     * @param {Object} replSet the ReplSetTest instance to adopt
     */
    function validateAndUseSetup(replSet) {
        assert.eq(true,
                  replSet instanceof ReplSetTest,
                  `Must provide an instance of ReplSetTest. Have: ${tojson(replSet)}`);

        assert.eq(true, replSet.usesBridge(), "Must set up ReplSetTest with mongobridge enabled.");
        assert.eq(3, replSet.nodes.length, "Replica set must contain exactly three nodes.");

        // Make sure we have a primary.
        curPrimary = replSet.getPrimary();

        // Extract the other two nodes and wait for them to be ready.
        let secondaries = replSet.getSecondaries();
        arbiter = replSet.getArbiter();
        curSecondary = (secondaries[0] === arbiter) ? secondaries[1] : secondaries[0];

        waitForState(curSecondary, ReplSetTest.State.SECONDARY);
        waitForState(arbiter, ReplSetTest.State.ARBITER);

        rst = replSet;
        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
    }

    /**
     * Return an instance of ReplSetTest initialized with a standard
     * three-node replica set running with the latest version.
     */
    function performStandardSetup() {
        let nodeOptions = {};
        if (TestData.logComponentVerbosity) {
            nodeOptions["setParameter"] = {
                "logComponentVerbosity": tojsononeline(TestData.logComponentVerbosity)
            };
        }

        let replSet = new ReplSetTest({name, nodes: 3, useBridge: true, nodeOptions: nodeOptions});
        replSet.startSet();

        const nodes = replSet.nodeList();
        replSet.initiate({
            _id: name,
            members: [
                {_id: 0, host: nodes[0]},
                {_id: 1, host: nodes[1]},
                {_id: 2, host: nodes[2], arbiterOnly: true}
            ]
        });

        assert.eq(replSet.nodes.length - replSet.getArbiters().length,
                  kNumDataBearingNodes,
                  "Mismatch between number of data bearing nodes and test configuration.");

        return replSet;
    }

    function checkDataConsistency() {
        assert.eq(curState,
                  State.kSteadyStateOps,
                  "Not in kSteadyStateOps state, cannot check data consistency");

        // We must wait for collection drops to complete so that we don't get spurious failures
        // in the consistency checks.
        rst.nodes.forEach(TwoPhaseDropCollectionTest.waitForAllCollectionDropsToComplete);

        const name = rst.name;
        // We must check counts before we validate since validate fixes counts. We cannot check
        // counts if unclean shutdowns occur.
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
     * return whether the cluster can transition from the current State to `newState`.
     * @private
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
     * Transition from a rollback state to a steady state. Operations applied in this phase will
     * be replicated to all nodes and should not be rolled back.
     */
    this.transitionToSteadyStateOperations = function() {

        // Ensure the secondary is connected. It may already have been connected from a previous
        // stage.
        log(`Ensuring the secondary ${curSecondary.host} is connected to the other nodes`);
        curSecondary.reconnect([curPrimary, arbiter]);

        // If we shut down the primary before the secondary begins rolling back against it, then
        // the secondary may get elected and not actually roll back. In that case we do not
        // check the RBID and just await replication.
        if (!TestData.rollbackShutdowns) {
            log(`Waiting for rollback to complete on ${curSecondary.host}`, true);
            let rbid = -1;
            assert.soon(() => {
                try {
                    rbid = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
                } catch (e) {
                    // Command can fail when sync source is being cleared.
                }
                // Fail early if the rbid is greater than lastRBID+1.
                assert.lte(rbid,
                           lastRBID + 1,
                           `RBID is too large. current RBID: ${rbid}, last RBID: ${lastRBID}`);

                return rbid === lastRBID + 1;

            }, "Timed out waiting for RBID to increment on " + curSecondary.host);
        } else {
            log(`Skipping RBID check on ${curSecondary.host} because shutdowns ` +
                `may prevent a rollback here.`);
        }

        rst.awaitSecondaryNodes();
        rst.awaitReplication();

        log(`Rollback on ${curSecondary.host} (if needed) and awaitReplication completed`, true);

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
     * Transition to the first stage of rollback testing, where we isolate the current primary so
     * its operations will eventually be rolled back.
     */
    this.transitionToRollbackOperations = function() {
        // Ensure previous operations are replicated. The current secondary will be used as the sync
        // source later on, so it must be up-to-date to prevent any previous operations from being
        // rolled back.
        rst.awaitSecondaryNodes();
        rst.awaitReplication();

        transitionIfAllowed(State.kRollbackOps);

        // Disconnect the current primary from the secondary so operations on it will eventually be
        // rolled back. But do not disconnect it from the arbiter so it can stay as the primary.
        log(`Isolating the primary ${curPrimary.host} from the secondary ${curSecondary.host}`);
        curPrimary.disconnect([curSecondary]);

        return curPrimary;
    };

    /**
     * Transition to the second stage of rollback testing, where we isolate the old primary and
     * elect the old secondary as the new primary. Then, operations can be performed on the new
     * primary so that that optimes diverge and previous operations on the old primary will be
     * rolled back.
     */
    this.transitionToSyncSourceOperationsBeforeRollback = function() {
        transitionIfAllowed(State.kSyncSourceOpsBeforeRollback);

        // Insert one document to ensure rollback will not be skipped.
        let dbName = "EnsureThereIsAtLeastOneOperationToRollback";
        assert.writeOK(curPrimary.getDB(dbName).ensureRollback.insert(
            {thisDocument: 'is inserted to ensure rollback is not skipped'}));

        log(`Isolating the primary ${curPrimary.host} so it will step down`);
        curPrimary.disconnect([curSecondary, arbiter]);

        log(`Waiting for the primary ${curPrimary.host} to step down`);
        try {
            // The stepdown freeze period is short because the node is disconnected from
            // the rest of the replica set, so it physically can't become the primary.
            curPrimary.adminCommand({replSetStepDown: 1, force: true});
        } catch (e) {
            // Stepdown may fail if the node has already started stepping down.
            print('Caught exception from replSetStepDown: ' + e);
        }

        waitForState(curPrimary, ReplSetTest.State.SECONDARY);

        log(`Reconnecting the secondary ${curSecondary.host} to the arbiter so it can be elected`);
        curSecondary.reconnect([arbiter]);

        log(`Waiting for the new primary ${curSecondary.host} to be elected`);
        assert.soonNoExcept(() => {
            const res = curSecondary.adminCommand({replSetStepUp: 1});
            return res.ok;
        });

        const newPrimary = rst.getPrimary();

        // As a sanity check, ensure the new primary is the old secondary. The opposite scenario
        // should never be possible with 2 electable nodes and the sequence of operations thus far.
        assert.eq(newPrimary, curSecondary, "Did not elect a new node as primary");
        log(`Elected the old secondary ${newPrimary.host} as the new primary`);

        // The old primary is the new secondary; the old secondary just got elected as the new
        // primary, so we update the topology to reflect this change.
        curSecondary = curPrimary;
        curPrimary = newPrimary;

        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;

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

        log(`Reconnecting the secondary ${curSecondary.host} so it'll go into rollback`);
        curSecondary.reconnect([curPrimary, arbiter]);

        return curPrimary;
    };

    this.stop = function() {
        checkDataConsistency();
        transitionIfAllowed(State.kStopped);
        return rst.stopSet();
    };

    this.getPrimary = function() {
        return curPrimary;
    };

    this.getSecondary = function() {
        return curSecondary;
    };

    this.restartNode = function(nodeId, signal) {
        assert(signal === SIGKILL || signal === SIGTERM, `Received unknown signal: ${signal}`);
        assert.gte(nodeId, 0, "Invalid argument to RollbackTest.restartNode()");

        const hostName = rst.nodes[nodeId].host;

        if (!TestData.rollbackShutdowns) {
            log(`Not restarting node ${hostName} because 'rollbackShutdowns' was not specified.`);
            return;
        }

        if (nodeId >= kNumDataBearingNodes) {
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
        rst.start(nodeId, {}, true /* restart */);

        // Ensure that the primary is ready to take operations before continuing. If both nodes are
        // connected to the arbiter, the primary may switch.
        curPrimary = rst.getPrimary();
        curSecondary = rst.getSecondary();
        assert.neq(curPrimary, curSecondary);
    };
}
