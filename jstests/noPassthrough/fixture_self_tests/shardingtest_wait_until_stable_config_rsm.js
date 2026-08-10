/**
 * Tests that ShardingTest.waitUntilStable() does not return until every router has rediscovered the
 * config server in its ReplicaSetMonitor.
 *
 * If it returns early, the next operation routed through mongos triggers a ShardRegistry reload that
 * fails with FailedToSatisfyReadPreference, because the config server is still marked Unknown even
 * though it is healthy and primary.
 *
 * Rather than racing a real config server restart -- which only loses the race on slow variants such
 * as aubsan, where mongod startup can take upwards of ten seconds -- this test blackholes mongos
 * from the config server with mongobridge, so the gap is deterministic. The blackhole is targeted at
 * mongos specifically, which matters because waitUntilStable() calls configRS.getPrimary() from the
 * shell and each shard primary must still be able to reach the config server.
 *
 * @tags: [
 *   # The test blackholes one specific config server primary and assumes it stays primary.
 *   does_not_support_stepdowns,
 * ]
 */
import "jstests/multiVersion/libs/multi_cluster.js";

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

describe("ShardingTest.waitUntilStable", function () {
    // Whether this client's ReplicaSetMonitor currently sees a reachable primary for the config server.
    const seesConfigPrimary = (conn, configSetName) => {
        const rsStats = conn.adminCommand("connPoolStats").replicaSets[configSetName];
        if (!rsStats) {
            return false;
        }
        return rsStats.hosts.some((h) => h.ok && h.ismaster);
    };

    before(() => {
        this.st = new ShardingTest({
            shards: 2,
            rs: {nodes: 2},
            mongos: 1,
            config: 1,
            useBridge: true,
        });
        this.st.configRS.awaitReplication();
        this.configSetName = this.st.configRS.name;
        this.configPrimary = this.st.configRS.getPrimary();

        assert.soon(
            () => seesConfigPrimary(this.st.s, this.configSetName),
            "mongos never discovered the config server primary on a healthy cluster",
        );

        // The first call to waitUntilStable() on a new cluster is slow (tens of seconds), because it
        // is what populates each shard's ReplicaSetMonitor view of the other shards in
        // connPoolStats. Warm that up so the measurement below reflects the steady-state cost.
        this.st.waitUntilStable();

        // Measure the steady-state cost of the barrier on a healthy cluster. The blackhole in the
        // test below must be held for longer than this, otherwise it would be lifted before the
        // barrier returns and the test could not observe a stale config RSM.
        const baselineStartMs = Date.now();
        this.st.waitUntilStable();
        const baselineMs = Date.now() - baselineStartMs;
        this.holdMillis = Math.max(10 * 1000, baselineMs * 3);
        jsTest.log.info("Measured healthy waitUntilStable() baseline", {
            baselineMs,
            holdMillis: this.holdMillis,
        });
    });

    after(() => {
        this.st.stop();
    });

    it("waits for mongos to rediscover the config server", () => {
        // Blackhole mongos from the config server primary in both directions, so mongos's
        // ReplicaSetMonitor cannot complete a 'hello' and holds the node in the Unknown state. The
        // shell's own connections, and the shards', are unaffected.
        this.configPrimary.discardMessagesFrom(this.st.s, 1.0);
        this.st.s.discardMessagesFrom(this.configPrimary, 1.0);

        assert.soon(
            () => !seesConfigPrimary(this.st.s, this.configSetName),
            "mongos never stopped seeing the config server as a reachable primary while it was " +
                "blackholed from the config server",
        );

        // Lift the blackhole after holdMillis from a parallel shell. waitUntilStable() must block
        // for at least this long, since the RSM cannot recover while the blackhole is in place.
        //
        // The parallel shell cannot use the MongoBridge objects from this shell, so it speaks the
        // mongobridge control protocol directly: a 'discardMessagesFrom' command tagged with
        // $forBridge, sent to a bridge's own port, naming the peer by the address that peer's
        // bridge forwards to.
        const liftBlackhole = startParallelShell(
            funWithArgs(
                function (holdMillis, configBridgePort, configDest, mongosBridgePort, mongosDest) {
                    sleep(holdMillis);
                    const stopDiscarding = (bridgePort, peerDest) => {
                        const controlConn = new Mongo("localhost:" + bridgePort);
                        assert.commandWorked(
                            controlConn.runCommand(
                                "test",
                                {
                                    discardMessagesFrom: 1,
                                    $forBridge: true,
                                    host: peerDest,
                                    loss: 0.0,
                                },
                                0,
                            ),
                        );
                    };
                    stopDiscarding(configBridgePort, mongosDest);
                    stopDiscarding(mongosBridgePort, configDest);
                },
                this.holdMillis,
                this.configPrimary.port,
                this.configPrimary.dest,
                this.st.s.port,
                this.st.s.dest,
            ),
            this.configPrimary.port,
        );

        const startMs = Date.now();
        this.st.waitUntilStable();
        const elapsedMs = Date.now() - startMs;
        // Sample the RSM state immediately, before joining the parallel shell below -- joining it
        // blocks until the blackhole has been lifted, after which the RSM recovers and the
        // evidence is gone.
        const sawConfigPrimaryOnReturn = seesConfigPrimary(this.st.s, this.configSetName);

        jsTest.log.info("waitUntilStable() returned", {elapsedMs, sawConfigPrimaryOnReturn});

        liftBlackhole();

        assert(
            sawConfigPrimaryOnReturn,
            "waitUntilStable() returned while mongos had not yet rediscovered the config server",
            {elapsedMs},
        );
        assert.gte(
            elapsedMs,
            this.holdMillis,
            "waitUntilStable() returned before the blackhole could have been lifted, so it did " +
                "not wait for the config server to be rediscovered",
            {elapsedMs, holdMillis: this.holdMillis},
        );
    });

    it("routes operations successfully once the barrier has returned", () => {
        // These require a ShardRegistry reload, which is what fails when the config server has not
        // been rediscovered.
        const testDB = this.st.s.getDB(jsTestName());
        assert.commandWorked(testDB.runCommand({create: "c"}));
        assert.commandWorked(testDB.runCommand({drop: "c"}));
    });
});
