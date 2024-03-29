/**
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
 * --------------------------------------------------
 * | STATE TRANSITION            | NETWORK TOPOLOGY |
 * |-------------------------------------------------
 * |  kSteadyStateOps            |       T          |
 * |                             |     /   \        |
 * |                             |    P1 -  S       |
 * |-----------------------------|------------------|
 * |  kRollbackOps               |       T          |
 * |                             |     /            |
 * |                             |    P1    S       |
 * |-----------------------------|------------------|
 * | kSyncSourceOpsBeforeRollback|       T          |
 * |                             |         \        |
 * |                             |    P1    P2      |
 * |-----------------------------|------------------|
 * | kSyncSourceOpsDuringRollback|        T         |
 * |                             |          \       |
 * |                             |     R  -  P2     |
 * |-------------------------------------------------
 * Note: 'T' refers to tiebreaker node, 'S' refers to secondary, 'P[n]' refers to primary in
 * nth term and 'R' refers to rollback node.
 *
 * Please refer to the various `transition*` functions for more information on the behavior
 * of each stage.
 */

import {CollectionValidator} from "jstests/hooks/validate_collections.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {restartServerReplication, stopServerReplication} from "jstests/libs/write_concern_util.js";
import {TwoPhaseDropCollectionTest} from "jstests/replsets/libs/two_phase_drops.js";
import {waitForState} from "jstests/replsets/rslib.js";

/**
 *
 * This fixture allows the user to optionally pass in a custom ReplSetTest
 * to be used for the test. The underlying replica set must meet the following
 * requirements:
 *      1. It must have exactly three nodes: A primary and two secondaries. One of the secondaries
 *         must be configured with priority: 0 so that it won't be elected primary. Throughout
 *         this file, this secondary will be referred to as the tiebreaker node.
 *      2. It must be running with mongobridge.
 *      3. Must initiate the replset with high election timeout to avoid unplanned elections in the
 *         rollback test.
 *
 * If the caller does not provide their own replica set, a standard three-node
 * replset will be initialized instead, with all nodes running the latest version.
 *
 * After the initial fixture setup, nodes may be added to the fixture using RollbackTest.add(),
 * provided they are non-voting nodes.  These nodes will not be checked for replication state or
 * progress until kSteadyStateOps, or if consistency checks are skipped in kSteadyStateOps, the end
 * of the test.  If voting nodes are added directly to the ReplSetTest, the results are undefined.
 *
 * @param {string} [optional] name the name of the test being run
 * @param {Object} [optional] replSet the ReplSetTest instance to adopt
 * @param {Object} [optional] nodeOptions command-line options to apply to all nodes in the replica
 *     set. Ignored if 'replSet' is provided.
 */
export function RollbackTest(name = "RollbackTest", replSet, nodeOptions) {
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
    const kNumDataBearingNodes = 3;
    const kElectableNodes = 2;
    const kRetryIntervalMS = 25;

    let awaitSecondaryNodesForRollbackTimeout;

    let rst;
    let curPrimary;
    let curSecondary;
    let tiebreakerNode;

    let curState = State.kSteadyStateOps;
    let lastRBID;

    // Make sure we have a replica set up and running.
    replSet = (replSet === undefined) ? performStandardSetup(nodeOptions) : replSet;

    // Runs the passed in cmdObj on db with a security token if multitenancy support is activated.
    let runWithTenantIfNeeded = (function() {
        const adminDB = replSet.getPrimary().getDB("admin");
        const featureFlagRequireTenantID = FeatureFlagUtil.isEnabled(adminDB, "RequireTenantID");
        const featureFlagSecurityToken = FeatureFlagUtil.isEnabled(adminDB, "SecurityToken");
        const multitenancyDoc =
            assert.commandWorked(adminDB.adminCommand({getParameter: 1, multitenancySupport: 1}));
        if (multitenancyDoc.hasOwnProperty("multitenancySupport") &&
            multitenancyDoc.multitenancySupport && featureFlagRequireTenantID &&
            featureFlagSecurityToken) {
            return function(dbConn, cmdObj) {
                const tenantId = ObjectId();
                dbConn.getMongo()._setSecurityToken(_createTenantToken({tenant: tenantId}));
                assert.commandWorked(dbConn.runCommand(cmdObj));
                dbConn.getMongo()._setSecurityToken(undefined);
            }
        } else {
            return function(dbConn, cmdObj) {
                assert.commandWorked(dbConn.runCommand(cmdObj));
            }
        }
    })();

    validateAndUseSetup(replSet);

    // Majority writes in the initial phase, before transitionToRollbackOperations(), should be
    // replicated to the syncSource node so they aren't lost when syncSource steps up. Ensure that
    // majority writes can be acknowledged only by syncSource, not by tiebreakerNode.
    jsTestLog(`Stopping replication on ${tiebreakerNode.host}`);
    stopServerReplication(tiebreakerNode);

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
        // The default WC is majority and we must use w:1 to be able to properly test rollback.
        assert.commandWorked(curPrimary.adminCommand(
            {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));
        replSet.awaitReplication();

        // Extract the other two nodes and wait for them to be ready.
        let secondaries = replSet.getSecondaries();
        let config = replSet.getReplSetConfigFromNode();

        // Make sure chaining is disabled, so that the tiebreaker cannot be used as a sync source.
        assert.eq(config.settings.chainingAllowed,
                  false,
                  "Must set up ReplSetTest with chaining disabled.");

        // Make sure electionTimeoutMillis is set to high value to avoid unplanned elections in
        // the rollback test.
        assert.gte(config.settings.electionTimeoutMillis,
                   ReplSetTest.kForeverMillis,
                   "Must initiate the replset with high election timeout");

        // Make sure the primary is not a priority: 0 node.
        assert.neq(0, config.members[0].priority);
        assert.eq(config.members[0].host, curPrimary.host);

        // Make sure that of the two secondaries, one is a priority: 0 node and the other is not.
        assert.neq(config.members[1].priority, config.members[2].priority);

        curSecondary = (config.members[1].priority !== 0) ? secondaries[0] : secondaries[1];
        tiebreakerNode = (config.members[2].priority === 0) ? secondaries[1] : secondaries[0];

        waitForState(curSecondary, ReplSetTest.State.SECONDARY);
        waitForState(tiebreakerNode, ReplSetTest.State.SECONDARY);

        // Make sync source selection faster.
        replSet.nodes.forEach((node) => {
            configureFailPoint(
                node, "forceBgSyncSyncSourceRetryWaitMS", {sleepMS: kRetryIntervalMS});
            setFastGetMoreEnabled(node);
        });

        rst = replSet;
        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;

        // Insert a document and replicate it to all 3 nodes so that any of the nodes can sync from
        // any other. If we do not do this, then due to initial sync timing and sync source
        // selection all nodes may not be guaranteed to have overlapping oplogs.
        const dbName = "EnsureAnyNodeCanSyncFromAnyOther";
        // To prevent losing this document due to unclean shutdowns, we need to
        // ensure the insert was replicated and written to the on-disk journal of all 3
        // nodes, with the exception of ephemeral and in-memory storage engines where
        // journaling isn't supported.

        runWithTenantIfNeeded(curPrimary.getDB(dbName), {
            insert: "ensureSyncSource",
            documents: [{thisDocument: 'is inserted to ensure any node can sync from any other'}],
            writeConcern: {w: 3, j: config.writeConcernMajorityJournalDefault}
        });
    }

    /**
     * We set the election timeout to 24 hours to prevent unplanned elections, but this has the
     * side-effect of causing `getMore` in replication to wait up 30 seconds prior to returning.
     *
     * The `setSmallOplogGetMoreMaxTimeMS` failpoint causes the `getMore` calls to block for a
     * maximum of 50 milliseconds.
     */
    function setFastGetMoreEnabled(node) {
        assert.commandWorked(
            node.adminCommand(
                {configureFailPoint: 'setSmallOplogGetMoreMaxTimeMS', mode: 'alwaysOn'}),
            `Failed to enable setSmallOplogGetMoreMaxTimeMS failpoint.`);
    }

    /**
     * Return an instance of ReplSetTest initialized with a standard
     * three-node replica set running with the latest version.
     *
     * Note: One of the secondaries will have a priority of 0.
     */
    function performStandardSetup(nodeOptions) {
        nodeOptions = nodeOptions || {};
        if (TestData.logComponentVerbosity) {
            nodeOptions["setParameter"] = nodeOptions["setParameter"] || {};
            nodeOptions["setParameter"]["logComponentVerbosity"] =
                tojsononeline(TestData.logComponentVerbosity);
        }
        if (TestData.syncdelay) {
            nodeOptions["syncdelay"] = TestData.syncdelay;
        }

        let replSet = new ReplSetTest({name, nodes: 3, useBridge: true, nodeOptions: nodeOptions});
        replSet.startSet();
        replSet.nodes.forEach(setFastGetMoreEnabled);

        let config = replSet.getReplSetConfig();
        config.members[2].priority = 0;
        config.settings = {chainingAllowed: false};
        replSet.initiateWithHighElectionTimeout(config);
        // Tiebreaker's replication is paused for most of the test, avoid falling off the oplog.
        replSet.nodes.forEach((node) => {
            assert.commandWorked(node.adminCommand({replSetResizeOplog: 1, minRetentionHours: 2}));
        });

        assert.eq(replSet.nodes.length,
                  kNumDataBearingNodes,
                  "Mismatch between number of data bearing nodes and test configuration.");

        return replSet;
    }

    // Track if we've done consistency checks.
    let doneConsistencyChecks = false;

    // This is an instance method primarily so it can be overridden in testing.
    this._checkDataConsistencyImpl = function() {
        assert.eq(curState,
                  State.kSteadyStateOps,
                  "Not in kSteadyStateOps state, cannot check data consistency");

        // We must wait for collection drops to complete so that we don't get spurious failures
        // in the consistency checks.
        rst.awaitSecondaryNodes();
        rst.nodes.forEach(TwoPhaseDropCollectionTest.waitForAllCollectionDropsToComplete);

        const name = rst.name;
        rst.checkOplogs(name);
        rst.checkPreImageCollection(name);
        rst.checkReplicatedDataHashes(name);
        collectionValidator.validateNodes(rst.nodeList());
    };

    this.checkDataConsistency = function() {
        doneConsistencyChecks = true;
        this._checkDataConsistencyImpl();
    };

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

    function stepUp(conn) {
        log(`Waiting for the new primary ${conn.host} to be elected`);
        assert.soonNoExcept(() => {
            const res = conn.adminCommand({replSetStepUp: 1});
            return res.ok;
        }, `failed to step up node ${conn.host}`, ReplSetTest.kDefaultTimeoutMS, kRetryIntervalMS);

        // Waits for the primary to accept new writes.
        return rst.getPrimary(ReplSetTest.kDefaultTimeoutMS, kRetryIntervalMS);
    }

    this.stepUpNode = function(conn) {
        stepUp(conn);
    };

    function oplogTop(conn) {
        return conn.getDB("local").oplog.rs.find().limit(1).sort({$natural: -1}).next();
    }

    /**
     * Add a node to the ReplSetTest.  It must be a non-voting node.  If reInitiate is true,
     * also run ReplSetTest.reInitiate to configure the replset to include the new node.
     */
    this.add = function({config: config, reInitiate: reInitiate = true}) {
        assert.eq(config.rsConfig.votes, 0, "Nodes added to a RollbackTest must be non-voting.");
        let node = rst.add(config);
        if (reInitiate) {
            rst.reInitiate();
        }
        // New node to do consistency checks on.
        // Note that this behavior isn't tested in rollbacktest_unittest.js.
        doneConsistencyChecks = false;
        return node;
    };

    /**
     * Transition from a rollback state to a steady state. Operations applied in this phase will
     * be replicated to all nodes and should not be rolled back.
     */
    this.transitionToSteadyStateOperations = function({skipDataConsistencyChecks = false} = {}) {
        const start = new Date();
        // Ensure rollback completes before reconnecting tiebreaker.
        //
        // 1. Wait for the rollback node to be SECONDARY; this either waits for rollback to finish
        // or exits early if it checks the node before it *enters* ROLLBACK.
        //
        // 2. Test that RBID is properly incremented; note that it could be incremented several
        // times if the node restarts before a given rollback attempt finishes.
        //
        // 3. Check if the rollback node is caught up.
        //
        // If any conditions are unmet, retry.
        //
        // If {enableMajorityReadConcern:false} is set, it will use the rollbackViaRefetch
        // algorithm. That can lead to unrecoverable rollbacks, particularly in unclean shutdown
        // suites, as it is possible in rare cases for the sync source to lose the entry
        // corresponding to the optime the rollback node chose as its minValid.
        log(`Wait for ${curSecondary.host} to finish rollback`);
        assert.soonNoExcept(
            () => {
                try {
                    log(`Wait for secondary ${curSecondary} and tiebreaker ${tiebreakerNode}`);
                    rst.awaitSecondaryNodesForRollbackTest(
                        awaitSecondaryNodesForRollbackTimeout,
                        [curSecondary, tiebreakerNode],
                        curSecondary /* connToCheckForUnrecoverableRollback */,
                        kRetryIntervalMS);
                } catch (e) {
                    if (e.unrecoverableRollbackDetected) {
                        log(`Detected unrecoverable rollback on ${curSecondary.host}. Ending test.`,
                            true /* important */);
                        TestData.skipCheckDBHashes = true;
                        rst.stopSet();
                        quit();
                    }
                    // Re-throw the original exception in all other cases.
                    throw e;
                }

                let rbid = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;
                assert(rbid > lastRBID,
                       `Expected RBID to increment past ${lastRBID} on ${curSecondary.host}`);

                assert.eq(oplogTop(curPrimary), oplogTop(curSecondary));

                return true;
            },
            `Waiting for rollback to complete on ${curSecondary.host} failed`,
            ReplSetTest.kDefaultTimeoutMS,
            kRetryIntervalMS);
        log(`Rollback on ${curSecondary.host} completed, reconnecting tiebreaker`, true);
        tiebreakerNode.reconnect([curPrimary, curSecondary]);

        // Allow replication temporarily so the following checks succeed.
        restartServerReplication(tiebreakerNode);

        rst.awaitReplication(null, null, [curSecondary, tiebreakerNode], kRetryIntervalMS);
        log(`awaitReplication completed`, true);

        // We call transition to steady state ops after awaiting replication has finished,
        // otherwise it could be confusing to see operations being replicated when we're already
        // in rollback complete state.
        transitionIfAllowed(State.kSteadyStateOps);

        // After the previous rollback (if any) has completed and await replication has finished,
        // the replica set should be in a consistent and "fresh" state. We now prepare for the next
        // rollback.
        if (skipDataConsistencyChecks) {
            print('Skipping data consistency checks');
        } else {
            this.checkDataConsistency();
        }

        // Now that awaitReplication and checkDataConsistency are done, stop replication again so
        // tiebreakerNode is never part of w: majority writes, see comment at top.
        stopServerReplication(tiebreakerNode, kRetryIntervalMS);
        log(`RollbackTest transition to ${curState} took ${(new Date() - start)} ms`);
        return curPrimary;
    };

    /**
     * Transition to the first stage of rollback testing, where we isolate the current primary so
     * that subsequent operations on it will eventually be rolled back.
     */
    this.transitionToRollbackOperations = function() {
        const start = new Date();

        // Ensure previous operations are replicated to the secondary that will be used as the sync
        // source later on. It must be up-to-date to prevent any previous operations from being
        // rolled back.
        rst.awaitSecondaryNodes(null, [curSecondary, tiebreakerNode]);
        rst.awaitReplication(null, null, [curSecondary]);

        transitionIfAllowed(State.kRollbackOps);

        // Disconnect the secondary from the tiebreaker node before we disconnect the secondary from
        // the primary to ensure that the secondary will be ineligible to win an election after it
        // loses contact with the primary.
        log(`Isolating the secondary ${curSecondary.host} from the tiebreaker
            ${tiebreakerNode.host}`);
        curSecondary.disconnect([tiebreakerNode]);

        // Disconnect the current primary, the rollback node, from the secondary so operations on
        // it will eventually be rolled back.
        // We do not disconnect the primary from the tiebreaker node so that it remains primary.
        log(`Isolating the primary ${curPrimary.host} from the secondary ${curSecondary.host}`);
        curPrimary.disconnect([curSecondary]);

        // We go through this phase every time a rollback occurs.
        doneConsistencyChecks = false;
        log(`RollbackTest transition to ${curState} took ${(new Date() - start)} ms`);
        return curPrimary;
    };

    /**
     * Transition to the second stage of rollback testing, where we isolate the old primary and
     * elect the old secondary as the new primary. Then, operations can be performed on the new
     * primary so that that optimes diverge and previous operations on the old primary will be
     * rolled back.
     */
    this.transitionToSyncSourceOperationsBeforeRollback = function() {
        const start = new Date();

        transitionIfAllowed(State.kSyncSourceOpsBeforeRollback);

        // Insert one document to ensure rollback will not be skipped. This needs to be journaled to
        // ensure that this document is not lost due to unclean shutdowns. Ephemeral and in-memory
        // storage engines are an exception because journaling isn't supported.
        let writeConcern = TestData.rollbackShutdowns ? {w: 1, j: true} : {w: 1};
        let dbName = "EnsureThereIsAtLeastOneOpToRollback";
        runWithTenantIfNeeded(curPrimary.getDB(dbName), {
            insert: "ensureRollback",
            documents: [{thisDocument: 'is inserted to ensure rollback is not skipped'}],
            writeConcern
        });

        log(`Isolating the primary ${curPrimary.host} so it will step down`);
        // We should have already disconnected the primary from the secondary during the first stage
        // of rollback testing.
        curPrimary.disconnect([tiebreakerNode]);

        log(`Waiting for the primary ${curPrimary.host} to step down`);
        try {
            // The stepdown freeze period is short because the node is disconnected from
            // the rest of the replica set, so it physically can't become the primary.
            assert.soon(() => {
                const res = curPrimary.adminCommand({replSetStepDown: 1, force: true});
                return (res.ok || res.code === ErrorCodes.NotWritablePrimary);
            });
        } catch (e) {
            // Stepdown may fail if the node has already started stepping down.
            print('Caught exception from replSetStepDown: ' + e);
        }

        waitForState(curPrimary, ReplSetTest.State.SECONDARY);

        log(`Reconnecting the secondary ${curSecondary.host} to the tiebreaker node so it can be
            elected`);
        curSecondary.reconnect([tiebreakerNode]);
        // Send out an immediate round of heartbeats to elect the node more quickly.
        assert.commandWorked(curSecondary.adminCommand({replSetTest: 1, restartHeartbeats: 1}));

        const newPrimary = stepUp(curSecondary);

        // As a sanity check, ensure the new primary is the old secondary. The opposite scenario
        // should never be possible with 2 electable nodes and the sequence of operations thus far.
        assert.eq(newPrimary, curSecondary, "Did not elect a new node as primary");
        log(`Elected the old secondary ${newPrimary.host} as the new primary`);

        // The old primary is the new secondary; the old secondary just got elected as the new
        // primary, so we update the topology to reflect this change.
        curSecondary = curPrimary;
        curPrimary = newPrimary;

        // To ensure rollback won't be skipped for shutdowns, wait till the no-op oplog
        // entry ("new primary") written in the new term gets persisted in the disk.
        // Note: rollbackShutdowns are not allowed for in-memory/ephemeral storage engines.
        if (TestData.rollbackShutdowns) {
            const dbName = "TermGetsPersisted";
            assert.commandWorked(curPrimary.getDB(dbName).ensureRollback.insert(
                {thisDocument: 'is inserted to ensure rollback is not skipped'},
                {writeConcern: {w: 1, j: true}}));
        }

        lastRBID = assert.commandWorked(curSecondary.adminCommand("replSetGetRBID")).rbid;

        log(`RollbackTest transition to ${curState} took ${(new Date() - start)} ms`);
        // The current primary, which is the old secondary, will later become the sync source.
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
        const start = new Date();
        transitionIfAllowed(State.kSyncSourceOpsDuringRollback);

        // Wait for expected states in case the secondary is starting up.
        rst.awaitSecondaryNodes(null, [curSecondary]);

        log(`Reconnecting the secondary ${curSecondary.host} so it'll go into rollback`);
        // Reconnect the rollback node to the current primary, which is the node we want to sync
        // from. If we reconnect to both the current primary and the tiebreaker node, the rollback
        // node may choose the tiebreaker. Send out a new round of heartbeats immediately so that
        // the rollback node can find a sync source quickly. If there was a network error when
        // trying to send out a new round of heartbeats, that indicates that rollback was already
        // in progress and had closed connections, so there's no need to retry the command.
        curSecondary.reconnect([curPrimary]);
        assert.adminCommandWorkedAllowingNetworkError(curSecondary,
                                                      {replSetTest: 1, restartHeartbeats: 1});

        log(`RollbackTest transition to ${curState} took ${(new Date() - start)} ms`);
        return curPrimary;
    };

    this.stop = function(checkDataConsistencyOptions, skipDataConsistencyCheck = false) {
        const start = new Date();
        restartServerReplication(tiebreakerNode);
        rst.awaitReplication();
        if (!doneConsistencyChecks && !skipDataConsistencyCheck) {
            this.checkDataConsistency(checkDataConsistencyOptions);
        }
        transitionIfAllowed(State.kStopped);
        log(`RollbackTest transition to ${curState} took ${(new Date() - start)} ms`);
        return rst.stopSet(undefined /* signal */,
                           undefined /* forRestart */,
                           {skipCheckDBHashes: true, skipValidation: true});
    };

    this.getPrimary = function() {
        return curPrimary;
    };

    this.getSecondary = function() {
        return curSecondary;
    };

    this.getTieBreaker = function() {
        return tiebreakerNode;
    };

    this.restartNode = function(nodeId, signal, startOptions, allowedExitCode) {
        assert(signal === SIGKILL || signal === SIGTERM, `Received unknown signal: ${signal}`);
        assert.gte(nodeId, 0, "Invalid argument to RollbackTest.restartNode()");

        const hostName = rst.nodes[nodeId].host;

        if (!TestData.rollbackShutdowns) {
            log(`Not restarting node ${hostName} because 'rollbackShutdowns' was not specified.`);
            return;
        }

        if (nodeId >= kElectableNodes) {
            log(`Not restarting node ${nodeId} because this replica set is too small or because
                we don't want to restart the tiebreaker node.`);
            return;
        }

        if (!TestData.allowUncleanShutdowns && signal !== SIGTERM) {
            log(`Sending node ${hostName} signal ${SIGTERM}` +
                ` instead of ${signal} because 'allowUncleanShutdowns' was not specified.`);
            signal = SIGTERM;
        }

        // We may attempt to restart a node while it is in rollback or recovery, in which case
        // the validation checks will fail. We will still validate collections during the
        // RollbackTest's full consistency checks, so we do not lose much validation coverage.
        let opts = {skipValidation: true};

        if (allowedExitCode !== undefined) {
            Object.assign(opts, {allowedExitCode: allowedExitCode});
        } else if (signal === SIGKILL) {
            Object.assign(opts, {allowedExitCode: MongoRunner.EXIT_SIGKILL});
        }

        log(`Stopping node ${hostName} with signal ${signal}`);
        rst.stop(nodeId, signal, opts, {forRestart: true});
        log(`Restarting node ${hostName}`);
        rst.start(nodeId, startOptions, true /* restart */);

        // Fail-point will clear on restart so do post-start.
        setFastGetMoreEnabled(rst.nodes[nodeId]);
        // Make sync source selection faster.
        configureFailPoint(
            rst.nodes[nodeId], "forceBgSyncSyncSourceRetryWaitMS", {sleepMS: kRetryIntervalMS});

        // Step up if the restarted node is the current primary.
        if (rst.getNodeId(curPrimary) === nodeId) {
            // To prevent below step up from being flaky, we step down and freeze the
            // current secondary to prevent starting a new election. The current secondary
            // can start running election due to explicit step up by the shutting down of current
            // primary if the server parameter "enableElectionHandoff" is set to true.
            rst.freeze(curSecondary);

            const newPrimary = stepUp(curPrimary);
            // As a sanity check, ensure the new primary is the current primary. This is true,
            // because we have configured the replica set with high electionTimeoutMillis.
            assert.eq(newPrimary, curPrimary, "Did not elect the same node as primary");

            // Unfreeze the current secondary so that it can step up again. Retry on network errors
            // in case the current secondary is in ROLLBACK state.
            assert.soon(() => {
                try {
                    assert.commandWorked(curSecondary.adminCommand({replSetFreeze: 0}));
                    return true;
                } catch (e) {
                    if (isNetworkError(e)) {
                        return false;
                    }
                    throw e;
                }
            }, `Failed to unfreeze current secondary ${curSecondary.host}`);
        }

        curSecondary = rst.getSecondary();
        assert.neq(curPrimary, curSecondary);

        waitForState(curSecondary, ReplSetTest.State.SECONDARY);
    };

    /**
     * Waits for the last oplog entry to be visible on all nodes except the tiebreaker, which has
     * replication stopped throughout the test.
     */
    this.awaitLastOpCommitted = function(timeout) {
        return rst.awaitLastOpCommitted(timeout, [curPrimary, curSecondary]);
    };

    /**
     * Waits until the optime of the specified type reaches the primary's last applied optime.
     * Ignores the tiebreaker node, on which replication is stopped throughout the test.
     * See ReplSetTest for definition of secondaryOpTimeType.
     */
    this.awaitReplication = function(timeout, secondaryOpTimeType) {
        return rst.awaitReplication(timeout, secondaryOpTimeType, [curPrimary, curSecondary]);
    };

    /**
     * Returns the underlying ReplSetTest in case the user needs to make adjustments to it.
     */
    this.getTestFixture = function() {
        return rst;
    };

    /**
     * Use this to control the timeout being used in the awaitSecondaryNodesForRollbackTest call
     * in transitionToSteadyStateOperations.
     * For use only in tests that expect unrecoverable rollbacks.
     */
    this.setAwaitSecondaryNodesForRollbackTimeout = function(timeoutMillis) {
        awaitSecondaryNodesForRollbackTimeout = timeoutMillis;
    };
}
