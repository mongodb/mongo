/**
 * Sets up and verifies $queryStats on all nodes in a sharded cluster, or on the given connection
 * (standalone or replica set primary) if not sharded.
 *
 * Operation mode enum:
 *   - "clear": Resets all Query Stats Store entries and the query stats cache size before a test.
 *   - "verify": Reads and verifies $queryStats entries after a test.
 */
import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import newMongoWithRetry from "jstests/libs/retryable_mongo.js";

assert.neq(typeof db, "undefined", "No `db` object, is the shell connected to a server?");

const conn = db.getMongo();

// This error code means that query stats is not enabled. This can happen in the fuzzer, since the
// fuzzer can set 'internalQueryStatsCacheSize' to 0.
const kQueryStatsStoreSize0ErrCode = 6579000;
// TODO SERVER-116389 remove allowFeatureNotSupported.
const allowFeatureNotSupported = TestData.allowFeatureNotSupported || false;

// HMAC key for transformIdentifiers (32 bytes of zeros).
const HMAC_KEY = HexData(8, "0".repeat(64));

function verifyQueryStats(nodeConn, queryStatsSpec) {
    try {
        const cursor = nodeConn.getDB("admin").aggregate([{"$queryStats": queryStatsSpec}]);
        while (cursor.hasNext()) {
            const operation = cursor.next();
            assert(operation.hasOwnProperty("key"), "Missing 'key' in $queryStats result");
            assert(operation.hasOwnProperty("metrics"), "Missing 'metrics' in $queryStats result");
            assert(operation.hasOwnProperty("asOf"), "Missing 'asOf' in $queryStats result");
        }
    } catch (e) {
        // The analyze command cannot be re-parsed (see SERVER-85374). Since this is a test only
        // command, it is low priority to fix this issue, and therefore we ignore the reparse errors
        // in this hook. The analyze command desugars into an aggregation pipeline that includes the
        // "system.statistics" collection and the '$_internalConstructStats' accumulator. Since the
        // aggregation pipeline can be slightly different based on user inputs, we do a best guess
        // here that we are running analyze.
        const analyzeCmdReparseErr =
            e.message.indexOf("Failed to re-parse query") !== -1 &&
            e.message.indexOf("$_internalConstructStats") !== -1 &&
            e.message.indexOf("system.statistics") !== -1;
        if (analyzeCmdReparseErr || (allowFeatureNotSupported && kQueryStatsStoreSize0ErrCode == e.code)) {
            jsTest.log.info("Encountered an error while running $queryStats. $queryStats will not run for this test.");
        } else {
            throw e;
        }
    }
}

function clearQueryStats(nodeConn) {
    // Clear out all existing entries, then reset the size cap.
    nodeConn.getDB("admin").runCommand({"setParameter": 1, internalQueryStatsCacheSize: "0%"});
    nodeConn.getDB("admin").runCommand({"setParameter": 1, internalQueryStatsCacheSize: "1%"});
}

function takeAction(conn, operation) {
    if (operation === "clear") {
        clearQueryStats(conn);
    } else {
        verifyQueryStats(conn, {});
        // Also verify with transformIdentifiers using the constant HMAC key.
        verifyQueryStats(conn, {
            transformIdentifiers: {
                algorithm: "hmac-sha-256",
                hmacKey: HMAC_KEY,
            },
        });
    }
}

let topology;
try {
    topology = DiscoverTopology.findConnectedNodes(conn);
} catch (e) {
    let errorWithCode = "Code: " + e.code + ", Message: " + e;
    jsTest.log.info("Error during topology discovery: " + errorWithCode);
    throw e;
}

const operation = TestData.queryStatsOperation || "verify";

// In all cases, run on the current connection (mongos or replica set).
takeAction(conn, operation);
if (topology.type === Topology.kShardedCluster) {
    // Sharded cluster - run on all shard nodes as well.
    try {
        for (let shardName of Object.keys(topology.shards)) {
            const shard = topology.shards[shardName];

            if (shard.type === Topology.kReplicaSet) {
                shard.nodes.forEach((node) => {
                    const nodeConn = newMongoWithRetry(node);
                    takeAction(nodeConn, operation);
                });
            } else if (shard.type === Topology.kStandalone) {
                const nodeConn = newMongoWithRetry(shard.mongod);
                takeAction(nodeConn, operation);
            }
        }
    } catch (e) {
        jsTest.log.info("Error while running $queryStats on shard nodes: " + e);
        throw e;
    }
}
