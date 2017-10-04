/**
 * Wrapper around ReplSetTest for testing rollback behavior. It allows the caller to easily
 * transition between stages of a rollback without having to configure the replset explicitly.
 *
 * This library exposes the following 3 sequential stages of rollback:
 * 1. RollbackTest starts in kSteadyStateOps: the replica set is in steady state replication.
 *        Operations applied will be replicated.
 * 2. kRollbackOps: operations applied during this phase will not be replicated and eventually be
 *        rolled back.
 * 3. kSyncSourceOps: operations applied during this stage will not be replicated initially,
 *        causing the data-bearing nodes to diverge. The data will be replicated after other
 *        nodes are rolled back.
 * 4. kSteadyStateOps: (back to stage 1) with the option of waiting for the rollback to finish.
 *
 * Please refer to the various `transition*` functions for more information on the behavior
 * of each stage.
 */
load("jstests/replsets/rslib.js");

function RollbackTest(name = "RollbackTest") {
    const State = {
        kStopped: "stopped",
        kRollbackOps: "doing operations that will be rolled back",
        kSyncSourceOps: "doing operations on sync source",
        kSteadyStateOps: "doing operations during steady state replication",
    };

    const AcceptableTransitions = {
        [State.kStopped]: [],
        [State.kRollbackOps]: [State.kSyncSourceOps],
        [State.kSyncSourceOps]: [State.kSteadyStateOps],
        [State.kSteadyStateOps]: [State.kStopped, State.kRollbackOps],
    };

    const rst = new ReplSetTest({name, nodes: 3, useBridge: true});

    let curPrimary;
    let curSecondary;
    let arbiter;
    let curState;

    // Do more complicated instantiation in this init() function. This reduces the amount of code
    // run at the class level (i.e. outside of any functions) and improves readability.
    (function init() {
        rst.startSet();

        const nodes = rst.nodeList();
        rst.initiate({
            _id: name,
            members: [
                {_id: 0, host: nodes[0]},
                {_id: 1, host: nodes[1]},
                {_id: 2, host: nodes[2], arbiterOnly: true}
            ]
        });

        // The primary is always the first node after calling ReplSetTest.initiate();
        curPrimary = rst.nodes[0];
        curSecondary = rst.nodes[1];
        arbiter = rst.nodes[2];

        // Wait for the primary to be ready.
        const actualPrimary = rst.getPrimary();

        assert.eq(actualPrimary, curPrimary, 'The ReplSetTest.node[0] is not the primary');

        curState = State.kSteadyStateOps;
    })();

    /**
     * return whether the cluster can transition from the current State to `newState`.
     * @private
     */
    function transitionIfAllowed(newState) {
        if (AcceptableTransitions[curState].includes(newState)) {
            jsTestLog(`[${name}] Transitioning to: "${newState}"`);
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
     *
     * @param waitForRollback specify whether to wait for rollback to finish before returning.
     */
    this.transitionToSteadyStateOperations = function(
        {waitForRollback: waitForRollback = true} = {}) {
        transitionIfAllowed(State.kSteadyStateOps);

        const originalRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;

        // Connect the secondary to everything else so it'll go into rollback.
        curSecondary.reconnect([curPrimary, arbiter]);

        if (waitForRollback) {
            jsTestLog(`[${name}] Waiting for rollback to complete`);
            assert.soonNoExcept(() => {
                // Command can fail when sync source is being cleared.
                const rbid = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
                return rbid === originalRBID + 1;
            }, 'Timed out waiting for RBID to increment');

            rst.awaitSecondaryNodes();
            rst.awaitReplication();

            jsTestLog(`[${name}] Rollback completed`);
        }

        return curPrimary;
    };

    /**
     * Transition to the first stage of rollback testing, where we isolate the current primary so
     * its operations will eventually be rolled back.
     */
    this.transitionToRollbackOperations = function() {
        transitionIfAllowed(State.kRollbackOps);

        // Ensure previous operations are replicated. The current secondary will be used as the sync
        // source later on, so it must be up-to-date to prevent any previous operations from being
        // rolled back.
        rst.awaitReplication();

        // Disconnect the current primary from the secondary so operations on it will eventually be
        // rolled back. But do not disconnect it from the arbiter so it can stay as the primary.
        curPrimary.disconnect([curSecondary]);

        return curPrimary;
    };

    /**
     * Transition to the second stage of rollback testing, where we isolate the old primary and
     * elect the old secondary as the new primary. Then, operations can be performed on the new
     * primary so that that optimes diverge and previous operations on the old primary will be
     * rolled back.
     */
    this.transitionToSyncSourceOperations = function() {
        transitionIfAllowed(State.kSyncSourceOps);

        // Isolate the current primary from everything so it steps down.
        curPrimary.disconnect([curSecondary, arbiter]);

        // Wait for the primary to step down.
        waitForState(curPrimary, ReplSetTest.State.SECONDARY);

        // Reconnect the secondary to the arbiter so it can be elected.
        curSecondary.reconnect([arbiter]);

        // Wait for the old secondary to become the new primary, which will be used as the sync
        // source later on when the old primary rolls back.
        const newPrimary = rst.getPrimary();

        // As a sanity check, ensure the new primary is the old secondary. The opposite scenario
        // should never be possible with 2 electable nodes and the sequence of operations thus far.
        assert.eq(newPrimary, curSecondary, "Did not elect a new node as primary");

        // Add a sleep and a dummy write to ensure the new primary has an optime greater than
        // the last optime on the node that will undergo rollback. This greater optime ensures that
        // the new primary is eligible to become a sync source in pv0.
        sleep(1000);
        var dbName = "ensureEligiblePV0";
        assert.writeOK(newPrimary.getDB(dbName).testColl.insert({id: 0}));

        // The old primary is the new secondary; the old secondary just got elected as the new
        // primary, so we update the topology to reflect this change.
        curSecondary = curPrimary;
        curPrimary = newPrimary;

        return curPrimary;
    };

    this.stop = function() {
        transitionIfAllowed(State.kStopped);
        const name = rst.name;
        rst.checkOplogs(name);
        rst.checkReplicatedDataHashes(name);
        return rst.stopSet();
    };

    this.getPrimary = function() {
        return curPrimary;
    };

    this.getSecondary = function() {
        return curSecondary;
    };
}
