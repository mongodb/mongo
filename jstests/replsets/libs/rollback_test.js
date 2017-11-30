/**
 * Wrapper around ReplSetTest for testing rollback behavior. It allows the caller to easily
 * transition between stages of a rollback without having to configure the replset explicitly.
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
load("jstests/replsets/rslib.js");

function RollbackTest(name = "RollbackTest") {
    const State = {
        kStopped: "kStopped",
        kRollbackOps: "kRollbackOps",
        // DEPRECATED: Remove this line after TIG-680.
        kSyncSourceOps: "kSyncSourceOpsBeforeRollback",
        kSyncSourceOpsBeforeRollback: "kSyncSourceOpsBeforeRollback",
        kSyncSourceOpsDuringRollback: "kSyncSourceOpsDuringRollback",
        kSteadyStateOps: "kSteadyStateOps",
    };

    const AcceptableTransitions = {
        [State.kStopped]: [],
        [State.kRollbackOps]: [State.kSyncSourceOpsBeforeRollback],
        // DEPRECATED: remove transition to State.kSteadyStateOps after TIG-680.
        [State.kSyncSourceOpsBeforeRollback]:
            [State.kSyncSourceOpsDuringRollback, State.kSteadyStateOps],
        [State.kSyncSourceOpsDuringRollback]: [State.kSteadyStateOps],
        [State.kSteadyStateOps]: [State.kStopped, State.kRollbackOps],
    };

    const rst = new ReplSetTest({name, nodes: 3, useBridge: true});

    let curPrimary;
    let curSecondary;
    let arbiter;

    let curState = State.kSteadyStateOps;
    let lastRBID;

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

        assert.eq(actualPrimary, curPrimary, "ReplSetTest.node[0] is not the primary");

        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
    })();

    function checkDataConsistency() {
        assert.eq(curState,
                  State.kSteadyStateOps,
                  "Not in kSteadyStateOps state, cannot check data consistency");
        const name = rst.name;
        rst.checkOplogs(name);
        rst.checkReplicatedDataHashes(name);
        // TODO: SERVER-31920 run validate.
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

        log("Waiting for rollback to complete", true);
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

        }, "Timed out waiting for RBID to increment");

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
        if (rst.getReplSetConfig().protocolVersion > 0) {
            assert.soonNoExcept(() => {
                const res = curSecondary.adminCommand({replSetStepUp: 1});
                return res.ok;
            });
        }

        const newPrimary = rst.getPrimary();

        // As a sanity check, ensure the new primary is the old secondary. The opposite scenario
        // should never be possible with 2 electable nodes and the sequence of operations thus far.
        assert.eq(newPrimary, curSecondary, "Did not elect a new node as primary");
        log(`Elected the old secondary ${newPrimary.host} as the new primary`);

        if (rst.getReplSetConfig().protocolVersion === 0) {
            // Add a sleep and a dummy write to ensure the new primary has an optime greater than
            // the last optime on the node that will undergo rollback. This greater optime ensures
            // that the new primary is eligible to become a sync source in pv0.
            sleep(1000);
            dbName = "ensureEligiblePV0";
            assert.writeOK(newPrimary.getDB(dbName).testColl.insert({id: 0}));
        }

        // The old primary is the new secondary; the old secondary just got elected as the new
        // primary, so we update the topology to reflect this change.
        curSecondary = curPrimary;
        curPrimary = newPrimary;

        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;

        return curPrimary;
    };

    // DEPRECATED: remove this line after TIG-680.
    this.transitionToSyncSourceOperations = this.transitionToSyncSourceOperationsBeforeRollback;

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
}
