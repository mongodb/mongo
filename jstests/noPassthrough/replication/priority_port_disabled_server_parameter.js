/*
 * Tests replication internals (such as heartbeats, initial sync, elections, etc.) once the
 * priority port is disabled via a server parameter.
 *
 * @tags: [
 *  featureFlagReplicationUsageOfPriorityPort,
 * ]
 */
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

Random.setRandomSeed();

describe("Tests for priority port usage within replication internals", function () {
    before(() => {
        let verbosity = {setParameter: {logComponentVerbosity: {replication: {heartbeats: 2}}}};
        this.rs = new ReplSetTest({
            nodes: 3,
            usePriorityPorts: true,
            nodeOptions: verbosity,
            settings: {
                chainingAllowed: false,
                heartbeatTimeoutSecs: 1,
                heartbeatIntervalMillis: 500,
                electionTimeoutMillis: 1000,
            },
        });
        this.rs.startSet();
        this.rs.initiate();

        this.fps = [];
        this.parameterNodes = [];

        // define some helper functions
        this.disablePriorityPortOnNode = function (node) {
            this.fps.push(
                configureFailPoint(node, "failCommand", {
                    failAllCommands: true,
                    priorityPortOnly: true,
                    failInternalCommands: true,
                    errorCode: ErrorCodes.HostUnreachable,
                }),
            );
        };

        this.setParameterOnNode = function (node) {
            this.parameterNodes.push(node);
            assert.commandWorked(node.adminCommand({setParameter: 1, disableReplicationUsageOfPriorityPort: true}));
        };
    });

    beforeEach(() => {
        // Step up a random node to ensure we have a primary during our tests.
        const secondaries = this.rs.getSecondaries();
        const newPrimaryIdx = Random.randInt(secondaries.length);
        const newPrimary = secondaries[newPrimaryIdx];
        this.rs.stepUp(newPrimary);
    });

    afterEach(() => {
        this.fps.forEach((fp) => {
            fp.off();
        });
        this.parameterNodes.forEach((node) => {
            assert.commandWorked(node.adminCommand({setParameter: 1, disableReplicationUsageOfPriorityPort: false}));
        });
        if (this.prioritiesModified) {
            let config = this.rs.getReplSetConfigFromNode();
            config.members.forEach((member) => {
                member.priority = 1;
            });
            config.version += 1;
            assert.commandWorked(this.rs.getPrimary().adminCommand({replSetReconfig: config}));
            this.prioritiesModified = false;
        }
    });

    after(() => {
        this.rs.stopSet();
    });

    function checkSyncingViaHostAndPorts(node, hostAndPorts) {
        assert.soon(() => {
            let status = assert.commandWorked(node.adminCommand({replSetGetStatus: 1}));
            return status.members.some((member) => {
                return member.self && hostAndPorts.includes(member.syncSourceHost);
            });
        });
    }

    it("Steady state replication", () => {
        let primary = this.rs.getPrimary();
        let secondaries = this.rs.getSecondaries();
        let fixedSecondary = secondaries[0];
        let blockedSecondary = secondaries[1];

        // Disallow blocked secondary from stepping up because it will not be able to hear back from
        // the primary thus causing it to run for an election.
        jsTest.log.info("Disallow the blocked secondary from stepping up");
        let config = this.rs.getReplSetConfigFromNode();
        config.members.forEach((member) => {
            if (member.host == blockedSecondary.host) {
                member.priority = 0;
            } else {
                member.priority = 1;
            }
        });
        config.version += 1;
        assert.commandWorked(primary.adminCommand({replSetReconfig: config}));
        this.prioritiesModified = true;

        jsTest.log.info("Block off the priority port so that nodes syncing via that port cannot progress");
        this.setParameterOnNode(fixedSecondary);

        jsTest.log.info("Check that our sync source is on the main port");
        let mainPortHosts = [];
        this.rs.nodes.forEach((node) => {
            mainPortHosts.push(node.host);
        });
        checkSyncingViaHostAndPorts(fixedSecondary, mainPortHosts);
        this.disablePriorityPortOnNode(primary);

        jsTest.log.info("Do some writes on the primary with local write concern");
        assert.commandWorked(primary.getDB("test").createCollection("foo"));
        for (let i = 0; i < 100; i++) {
            assert.commandWorked(
                primary.getDB("test").runCommand({insert: "foo", documents: [{x: i}], writeConcern: {w: 1}}),
            );
        }

        jsTest.log.info(
            "Check that we succcessfully replicate these writes to the secondary that has the server parameter set",
        );
        assert.soon(() => {
            let unblockedCount = fixedSecondary.getDB("test").getCollection("foo").count();
            let blockedCount = blockedSecondary.getDB("test").getCollection("foo").count();
            // The blocked secondary cannot replicate via the priority port, and it will not fall
            // back to the main port without the `disableReplicationUsageOfPriorityPort` server
            // parameter set.
            return unblockedCount == 100 && blockedCount == 0;
        });

        jsTest.log.info("Check that our sync source is still on the main port");
        checkSyncingViaHostAndPorts(fixedSecondary, mainPortHosts);
    });

    it("Election after disabling priority ports", () => {
        // We wait for replication here because we will block off one of the secondaries below and
        // the ReplSetTest helpers expect all secondaries to catch up in awaitReplication()
        jsTest.log.info("Wait for replication");
        this.rs.waitForStepUpWrites();
        this.rs.awaitReplication();

        jsTest.log.info(
            "Block priority port on current primary and on one of the secondaries and set the parameter on the other secondary",
        );
        let secondaries = this.rs.getSecondaries();
        let newPrimary = secondaries[0];
        this.setParameterOnNode(newPrimary);
        this.disablePriorityPortOnNode(this.rs.getPrimary());
        this.disablePriorityPortOnNode(secondaries[1]);

        jsTest.log.info("Step up the secondary with the parameter set");
        this.rs.stepUp(newPrimary, {awaitReplicationBeforeStepUp: false});
    });

    it("Heartbeats via priority port", () => {
        let primary = this.rs.getPrimary();

        jsTest.log.info("Switch the parameter on on the primary and block the secondaries's priority ports");
        this.setParameterOnNode(primary);
        this.rs.getSecondaries().forEach((secondary) => {
            this.disablePriorityPortOnNode(secondary);
        });

        jsTest.log.info("Ensure that the primary doesn't step down due to no heartbeat responses");
        clearRawMongoProgramOutput();
        let waitForHeartbeats = 10;
        const heartbeatResponseReceivedLogID = '"id":4615620';
        const heartbeatRequestSentLogID = '"id":4615670';
        assert.soon(() => {
            return (
                rawMongoProgramOutput(heartbeatResponseReceivedLogID).split("\n").length >= waitForHeartbeats &&
                rawMongoProgramOutput(heartbeatRequestSentLogID).split("\n").length >= waitForHeartbeats
            );
        });

        assert.soon(() => {
            let res = assert.commandWorked(primary.adminCommand({replSetGetStatus: 1}));
            return res.members.some((member) => {
                return member.self && member.state === ReplSetTest.State.PRIMARY;
            });
        });
    });
});
