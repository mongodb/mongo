/**
 * Verifies an unauthenticated client cannot advance a shard's logical clock by relaying a
 * genuinely, validly-signed $clusterTime observed on another shard that has progressed further
 * ahead. A valid signature only proves a $clusterTime came from this cluster at some point, not
 * that the presenter is authorized to advance the clock with it. This holds whether or not the
 * command carrying the relayed time requires authentication. In noPassthrough because auth is
 * manually set.
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */
import {ShardingTest} from "jstests/libs/shardingtest.js";

const keyFile = "jstests/libs/key1";
const st = new ShardingTest({mongos: 1, shards: 2, other: {keyFile}});

// Creating a user disables the localhost auth bypass cluster-wide.
st.s.getDB("admin").createUser({user: "foo", pwd: "bar", roles: jsTest.adminUserRoles});
assert(st.s.getDB("admin").auth("foo", "bar"));

const laggingShardPrimary = st.rs0.getPrimary();
const aheadShardPrimary = st.rs1.getPrimary();

const before = assert.commandWorked(
    laggingShardPrimary.getDB("admin").runCommand({hello: 1}),
).$clusterTime;

// Push the other shard's clusterTime into the future. Internal cluster members (__system, via
// the shared keyFile) are always authorized to advance the clock, even with an unsigned
// $clusterTime.
const futureTime = new Timestamp(before.clusterTime.getTime() + 1000, 0);
authutil.asCluster(aheadShardPrimary, keyFile, () => {
    assert.commandWorked(
        aheadShardPrimary
            .getDB("admin")
            .runCommand({hello: 1, $clusterTime: {clusterTime: futureTime}}),
    );
});

// The shard now signs futureTime as its own current time with its real key. Both shards share
// the same signing keys (distributed by the config server), so this signature is genuinely
// valid on the other shard too -- what an attacker could exploit by relaying it.
const attackerObservedTime = assert.commandWorked(
    aheadShardPrimary.getDB("admin").runCommand({hello: 1}),
).$clusterTime;
assert.gt(
    attackerObservedTime.clusterTime.getTime(),
    before.clusterTime.getTime(),
    "expected the other shard's clusterTime to be later than the lagging shard's baseline",
);

// Relay it to the lagging shard from a brand new, never-authenticated connection. A command that
// doesn't require authentication succeeds, with the relayed time simply ignored.
const unauthConn = new Mongo(laggingShardPrimary.host);
assert.commandWorked(
    unauthConn.getDB("admin").runCommand({hello: 1, $clusterTime: attackerObservedTime}),
    "expected an unauthenticated hello carrying a foreign, but validly-signed, cluster time to " +
        "still succeed",
);

// Commands that do require authentication are no way around the check above either, and for a
// stronger reason: CommandHelpers::uassertShouldAttemptParse rejects the unauthenticated client
// while parsing, before the request metadata carrying the relayed time is read at all, so the
// vector clock never even sees it.
assert.commandFailedWithCode(
    unauthConn.getDB("test").runCommand({find: "foo", $clusterTime: attackerObservedTime}),
    ErrorCodes.Unauthorized,
);

// The lagging shard's clock must not have jumped forward to the relayed value.
const relayReply = assert.commandWorked(
    unauthConn.getDB("admin").runCommand({hello: 1, $clusterTime: attackerObservedTime}),
);
assert.lt(
    relayReply.$clusterTime.clusterTime.getTime(),
    attackerObservedTime.clusterTime.getTime(),
    "unauthenticated client was able to advance the shard's clusterTime",
);

st.stop();
