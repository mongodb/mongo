/**
 * Redirects the global `db` connection straight to shard0's primary mongod, bypassing mongos.
 * Mirrors a client (e.g. Atlas Search's mongot) that connects to a shard directly. The original
 * mongos connection is preserved as `globalThis.routerConn`.
 *
 * Also retries on StaleConfig and other errors mongos would otherwise retry transparently, e.g.
 * right after a DDL op racing the shard's metadata recovery.
 */
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {retryDirectShardCommand} from "jstests/libs/override_methods/query/direct_to_shard_retry_util.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";

// A parallel shell may already be started with --host pointing directly at the shard, in which
// case there is no router topology to discover and no redirect to perform.
if (db.getMongo().isMongos()) {
    const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
    const shardName = Object.keys(topology.shards)[0];
    const shardPrimary = topology.shards[shardName].primary;

    globalThis.routerConn = db.getMongo();
    globalThis.db = new Mongo(shardPrimary).getDB(db.getName());
}

// Lets tests detect they're connected directly to a shard.
TestData.connectedDirectlyToShard = true;

function retryOnTransientErrors(conn, dbName, cmdName, cmdObj, func, makeFuncArgs) {
    return retryDirectShardCommand(cmdName, () => func.apply(conn, makeFuncArgs(cmdObj)));
}

OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query/implicit_direct_to_shard_connection.js",
);
OverrideHelpers.overrideRunCommand(retryOnTransientErrors);
