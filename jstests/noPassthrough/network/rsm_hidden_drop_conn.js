/**
 * Hiding a replica set member and then unhiding it must leave that member pingable on a mongos that
 * was already monitoring it.
 *
 * @tags: [
 *   requires_sharding,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {reconfig} from "jstests/replsets/rslib.js";

const kMaxRealPingMillis = 1e9; // real ping times are tiny; sentinel is ~9.2e15
const kPingRecoveryTimeoutMillis = 60 * 1000;
const kMembershipChangeTimeoutMillis = 2 * kPingRecoveryTimeoutMillis;

/**
 * Returns the ReplicaSetMonitor's view of `setName` from `mongos`'s connPoolStats.
 *
 * The return value is an array of `{addr, ok, ismaster, secondary, pingTimeMillis}`. `addr` is the
 * host, `ok`/`ismaster`/`secondary` describe how the monitor classifies it, and `pingTimeMillis`
 * is a real round-trip time in milliseconds, or the max sentinel (~9.2e15) when the host has no
 * ping monitor. The array is empty when that replica set is absent from connPoolStats.
 */
function getPingTimeView(mongos, setName) {
    const stats = assert.commandWorked(mongos.adminCommand({connPoolStats: 1}));
    const rs = stats.replicaSets && stats.replicaSets[setName];
    if (!rs || !rs.hosts) {
        return [];
    }
    return rs.hosts.map((h) => ({
        addr: h.addr,
        ok: h.ok,
        ismaster: h.ismaster,
        secondary: h.secondary,
        pingTimeMillis: h.pingTimeMillis,
    }));
}

function logPingTimeViews(label, mongos0, mongos1, setName) {
    jsTest.log.info(label, {
        mongos0: getPingTimeView(mongos0, setName),
        mongos1: getPingTimeView(mongos1, setName),
    });
}

/**
 * Returns the shard's connection string from config.shards (`"<set>/<host>[:port],..."`), or
 * `"<none>"` when no shard document is present.
 */
function getShardHosts(mongos) {
    const shardDoc = mongos.getDB("config").shards.findOne();
    return shardDoc ? shardDoc.host : "<none>";
}

/**
 * Address the ReplicaSetMonitor uses for `node`.
 *
 * With --shellGRPC, `node.host` is the shell's gRPC port. The replica set config and connPoolStats
 * use the listen port, which is `node.name`.
 */
function memberHost(node) {
    return node.name;
}

function setHidden(rs, host, hidden) {
    const config = rs.getReplSetConfigFromNode();
    const member = config.members.find((m) => m.host === host);
    assert(member, "member not found in config", {host, members: config.members});
    member.hidden = hidden;
    if (hidden) {
        member.priority = 0;
    }
    reconfig(rs, config);
}

/** Returns true when `actualHosts` and `expectedHosts` contain the same hosts, ignoring order. */
function sameHostList(actualHosts, expectedHosts) {
    const actual = actualHosts.slice().sort();
    const expected = expectedHosts.slice().sort();
    return actual.length === expected.length && actual.every((host, i) => host === expected[i]);
}

/**
 * Returns true when `mongos` is monitoring exactly `expectedHosts` and each has a real ping time
 * rather than the max sentinel.
 */
function seesHostsWithRealPings(mongos, setName, expectedHosts) {
    const view = getPingTimeView(mongos, setName);
    return (
        sameHostList(
            view.map((host) => host.addr),
            expectedHosts,
        ) && view.every((host) => host.pingTimeMillis < kMaxRealPingMillis)
    );
}

TestData.skipCheckDBHashes = true;
TestData.skipCheckShardFilteringMetadata = true;

describe("ReplicaSetMonitor ping time after hiding and unhiding a member", function () {
    before(function () {
        this.st = new ShardingTest({
            shards: {rs0: {nodes: 3}},
            mongos: 2,
            initiateWithDefaultElectionTimeout: true,
        });
        this.rs = this.st.rs0;
        this.mongos0 = this.st.s0;
        this.mongos1 = this.st.s1;
        this.setName = this.rs.name;
        this.hiddenHost = memberHost(this.rs.getSecondaries()[0]);

        jsTest.log.info("Created shard replica set", {
            setName: this.setName,
            members: this.rs.nodes.map(memberHost),
            hiddenHost: this.hiddenHost,
        });
    });

    after(function () {
        if (this.st) {
            this.st.stop({parallelSupported: false, skipValidation: true});
        }
    });

    it("gives the unhidden member a real ping time on a mongos that already knew it", function () {
        const {rs, setName, hiddenHost} = this;
        let mongos0 = this.mongos0;
        let mongos1 = this.mongos1;
        const allHosts = rs.nodes.map(memberHost);
        // The two members that stay visible while `hiddenHost` is hidden.
        const visibleHosts = allHosts.filter((host) => host !== hiddenHost);
        const sees = (mongos, expectedHosts) =>
            seesHostsWithRealPings(mongos, setName, expectedHosts);

        // Both mongoses should see every member with a real ping time.
        assert.soon(
            () => sees(mongos0, allHosts) && sees(mongos1, allHosts),
            "expected both mongoses to see every member with a real ping time",
            kMembershipChangeTimeoutMillis,
        );
        logPingTimeViews("Baseline ping times", mongos0, mongos1, setName);

        jsTest.log.info("Hiding member", {hiddenHost, visibleHosts});
        setHidden(rs, hiddenHost, true);
        assert.soon(
            () => sees(mongos0, visibleHosts),
            "expected mongos0 to see only the non-hidden members, each with a real ping time",
            kMembershipChangeTimeoutMillis,
        );
        jsTest.log.info("config.shards host after hide", {shardHost: getShardHosts(mongos0)});

        // Restart mongos1 so it creates its ReplicaSetMonitor from config.shards, which no longer
        // lists the hidden member, so it has no pooled connection to that member.
        jsTest.log.info("Restarting mongos1");
        this.st.restartMongos(1);
        mongos1 = this.st.s1;

        assert.soon(
            () => sees(mongos0, visibleHosts) && sees(mongos1, visibleHosts),
            "expected both mongoses to see only the non-hidden members after restarting mongos1",
            kMembershipChangeTimeoutMillis,
        );
        logPingTimeViews("Ping times after mongos1 restart", mongos0, mongos1, setName);

        jsTest.log.info("Unhiding member", {hiddenHost, allHosts});
        setHidden(rs, hiddenHost, false);
        // The unhidden member is added with the sentinel ping time until a new connection handshake
        // creates its SingleServerPingMonitor. That happens within about one heartbeat once the
        // pooled connection was dropped at hide time.
        assert.soon(
            () => sees(mongos0, allHosts) && sees(mongos1, allHosts),
            "expected both mongoses to see every member with a real ping time after unhide",
            kMembershipChangeTimeoutMillis + kPingRecoveryTimeoutMillis,
        );
        logPingTimeViews("Unhidden member is pingable", mongos0, mongos1, setName);
    });
});
