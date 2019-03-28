/**
 * Wrapper around ReplSetTest for testing initial sync behavior. It allows the caller to easily
 * pause in the middle of initial sync to allow the sync source to run commands.
 *
 * The test fixture pauses the secondary during both the collection cloning and oplog application
 * phases of initial sync. During collection cloning, the secondary is paused before each time it
 * would reach out to the sync source. During oplog application, the secondary is paused before
 * applying each batch of oplog entries. This enables a test to run commands on the sync source at a
 * deterministic point of the initial sync process and have the effects be observed by the
 * secondary. For example, this means that a test can exercise what happens if a collection returned
 * by the listCollections command is dropped before a cursor is established on it.
 *
 */

"use strict";

load("jstests/libs/check_log.js");
load('jstests/replsets/rslib.js');

/**
 * This fixture allows the user to optionally pass in a custom ReplSetTest to be used for the test.
 * The underlying replica set must have exactly two nodes: a primary, and a secondary.
 *
 * If the caller does not provide their own replica set, a two node replset will be initialized
 * instead, with all nodes running the latest version.
 *
 * @param {string} [name] the name of the test being run
 * @param {Object} [replSet] the ReplSetTest instance to adopt
 * @param {int} [timeout] how long to wait for initial sync to start
 */
function InitialSyncTest(name = "InitialSyncTest", replSet, timeout) {
    const State = {
        kBeforeInitialSync: "kBeforeInitialSync",
        kDuringInitialSync: "kDuringInitialSync",
        kInitialSyncCompleted: "kInitialSyncCompleted",
        kStopped: "kStopped"
    };

    const AcceptableTransitions = {
        [State.kBeforeInitialSync]: [State.kDuringInitialSync],
        [State.kDuringInitialSync]: [State.kInitialSyncCompleted],
        [State.kInitialSyncCompleted]: [State.kStopped],
        [State.kStopped]: []
    };

    let currState = State.kBeforeInitialSync;

    // Make sure we have a replica set up and running.
    replSet = (replSet === undefined) ? performSetup() : replSet;

    assert.eq(2, replSet.nodes.length, "Replica set must contain exactly two nodes.");

    let initialSyncTimeout = timeout || replSet.kDefaultTimeoutMS;

    const primary = replSet.getPrimary();
    let secondary = replSet.getSecondary();

    replSet.waitForState(secondary, ReplSetTest.State.SECONDARY);

    /**
     * Return an instance of ReplSetTest initialized with a standard two-node replica set running
     * with the latest version.
     */
    function performSetup() {
        let nodeOptions = {};
        if (TestData.logComponentVerbosity) {
            nodeOptions["setParameter"] = {
                "logComponentVerbosity": tojsononeline(TestData.logComponentVerbosity)
            };
        }

        let replSet = new ReplSetTest({
            name: name,
            nodes: [{}, {rsConfig: {priority: 0, votes: 0}}],
            nodeOptions: nodeOptions
        });
        replSet.startSet();
        replSet.initiate();

        return replSet;
    }

    /**
     * Transition from the current State to `newState` if it's a valid transition, otherwise throw
     * an error.
     */
    function transitionIfAllowed(newState) {
        if (AcceptableTransitions[currState].includes(newState)) {
            jsTestLog(`Transitioning to: "${newState}"`, true);
            currState = newState;
        } else {
            // Transitioning to a disallowed State is likely a bug in the code, so we throw an
            // error here instead of silently failing.
            throw new Error(`Can't transition to State "${newState}" from State "${currState}"`);
        }
    }

    /**
     * Calls replSetGetStatus and checks if the node is in the provided state.
     */
    function isNodeInState(node, state) {
        return state ===
            assert
                .commandWorkedOrFailedWithCode(node.adminCommand({replSetGetStatus: 1}),
                                               ErrorCodes.NotYetInitialized)
                .myState;
    }

    function hasStartedInitialSync() {
        // We know that initial sync has started once the node has transitioned to STARTUP2.
        return isNodeInState(secondary, ReplSetTest.State.STARTUP_2);
    }

    function hasCompletedInitialSync() {
        // Make sure this isn't called before the secondary starts initial sync.
        assert.eq(currState,
                  State.kDuringInitialSync,
                  "Should not check if initial sync completed before node is restarted");

        // We know initial sync has completed if the node has transitioned to SECONDARY state.
        return isNodeInState(secondary, ReplSetTest.State.SECONDARY);
    }

    function checkDataConsistency() {
        const name = replSet.name;

        // Make sure there are no open transactions.
        let status = assert.commandWorked(primary.adminCommand('serverStatus'));
        assert(typeof status.transactions === "object", status);
        assert.eq(0, status.transactions.currentOpen, status.transactions);

        // We must check counts before validate is called during stopSet since validate fixes
        // counts.
        replSet.checkCollectionCounts(name);
    }

    /**
     * Restarts the secondary with the first synchronization failpoint enabled so that we ensure
     * that initial sync pauses the first time the node reaches out to the sync source.
     */
    function restartNodeWithoutData() {
        // Skip validation when stopping the node in case there are transactions in prepare.
        replSet.stop(secondary, undefined, {skipValidation: true});

        const nodeOptions = {
            startClean: true,
            setParameter: {
                'failpoint.initialSyncFuzzerSynchronizationPoint1': tojson({mode: 'alwaysOn'}),
            },
        };

        if (TestData.logComponentVerbosity) {
            nodeOptions.setParameter.logComponentVerbosity =
                tojsononeline(TestData.logComponentVerbosity);
        }

        // Restart the node with the first synchronization failpoint enabled so that initial sync
        // doesn't finish before we can pause it.
        secondary = replSet.start(secondary, nodeOptions, true);
    }

    /**
     * Wait until the first synchronization fail point is hit to show that initial sync is paused
     * or until initial sync has completed.
     */
    function waitUntilInitialSyncPausedOrCompleted() {
        assert.soon(function() {
            if (checkLog.checkContainsOnce(
                    secondary, "initialSyncFuzzerSynchronizationPoint1 fail point enabled")) {
                return true;
            }
            return hasCompletedInitialSync();

        }, "initial sync did not pause or complete");
    }

    /**
     * Flip failpoints and wait until the second synchronization failpoint is hit so that we know
     * it is safe to let initial sync resume again. This step is necessary before issuing the next
     * command to ensure that we only run one command before pausing at the first synchronization
     * failpoint again.
     */
    function pauseBeforeSyncSourceCommand() {
        assert.commandWorked(secondary.adminCommand(
            {"configureFailPoint": 'initialSyncFuzzerSynchronizationPoint2', "mode": 'alwaysOn'}));
        assert.commandWorked(secondary.adminCommand(
            {"configureFailPoint": 'initialSyncFuzzerSynchronizationPoint1', "mode": 'off'}));
        checkLog.contains(secondary, "initialSyncFuzzerSynchronizationPoint2 fail point enabled");
    }

    /**
     * Flip failpoints and wait until the first synchronization failpoint is hit so that initial
     * sync can make progress by issuing the next command, but pausing before the following command
     * can be issued.
     */
    function resumeAndPauseBeforeNextSyncSourceCommand() {
        assert.commandWorked(secondary.adminCommand(
            {"configureFailPoint": 'initialSyncFuzzerSynchronizationPoint1', "mode": 'alwaysOn'}));
        assert.commandWorked(secondary.adminCommand(
            {"configureFailPoint": 'initialSyncFuzzerSynchronizationPoint2', "mode": 'off'}));

        waitUntilInitialSyncPausedOrCompleted();
    }

    /**
     * This function will resume initial sync and run the next command before using the
     * synchronization failpoints to make sure initial sync is either paused or completed. Other
     * than before initial sync has started and after initial sync has completed, when this
     * function is called the secondary should be paused at the first synchronization failpoint
     * before and after running the next command. This ensures that we can deterministically pause
     * initial sync and only one command is run each function call. During collection cloning, we
     * pause before running listDatabases, listCollections and listIndexes commands on the sync
     * source. During the oplog application phase, we pause before applying each batch of oplog
     * entries on the initial syncing node.
     *
     * If initial sync hasn't started yet, the function will restart the secondary without data to
     * cause it to go through initial sync. It will throw an exception if called after initial sync
     * has already completed or stop() has been called.
     *
     * @return true if initial sync has completed
     */
    this.step = function() {
        // If initial sync has not started yet, restart the node without data to cause it to go
        // through initial sync.
        if (currState === State.kBeforeInitialSync) {
            restartNodeWithoutData();

            // Wait until initial sync has started.
            assert.soon(hasStartedInitialSync, "failed to start initial sync", initialSyncTimeout);
            transitionIfAllowed(State.kDuringInitialSync);

            checkLog.contains(secondary,
                              "initialSyncFuzzerSynchronizationPoint1 fail point enabled");

            return false;
        }

        // Make sure this wasn't called after the test fixture was stopped or this function already
        // returned that initial sync was completed.
        assert.neq(currState, State.kStopped, "Cannot call step() if the test fixture was stopped");
        assert.neq(currState,
                   State.kInitialSyncCompleted,
                   "Cannot call step() if initial sync already completed");

        pauseBeforeSyncSourceCommand();

        // Clear ramlog so checkLog can't find log messages from previous times either failpoint was
        // enabled.
        assert.commandWorked(secondary.adminCommand({clearLog: 'global'}));

        resumeAndPauseBeforeNextSyncSourceCommand();

        // If initial sync is completed, let the caller know.
        if (hasCompletedInitialSync()) {
            transitionIfAllowed(State.kInitialSyncCompleted);
            assert.commandWorked(secondary.adminCommand(
                {"configureFailPoint": 'initialSyncFuzzerSynchronizationPoint1', "mode": 'off'}));
            return true;
        }

        return false;
    };

    this.getPrimary = function() {
        return primary;
    };

    this.getSecondary = function() {
        return secondary;
    };

    /**
     * Performs data consistency checks and then stops the replica set. Will fail if there is a
     * transaction that wasn't aborted or committed.
     */
    this.stop = function() {
        transitionIfAllowed(State.kStopped);
        checkDataConsistency();
        return replSet.stopSet();
    };
}