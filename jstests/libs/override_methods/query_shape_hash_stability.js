/**
 * Overrides Mongo.prototype.runCommand for query settings supported commands in order to run
 * explain on them multiple times in a row and ensure that the reported 'queryShapeHash' value is
 * same.
 **/
import {getCommandName, getExplainCommand, getInnerCommand} from "jstests/libs/cmd_object_utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

// Flag which tracks if we run this test using the two-cluster fixture.
const isMultiShardedClusterFixture = TestData.isMultiShardedClusterFixture || false;

/**
 * Because the _defaultSession is shared, spawning two mongo shells with the same app name uses a
 * cached ReplicaSetMonitor, which leads to only using the mongod's from the first cluster.
 */
const connectFn = (host) => {
    const conn = new Mongo(host, undefined, {gRPC: false});
    conn._defaultSession = new _DummyDriverSession(conn);
    conn.setSecondaryOk();
    return conn;
};

// Because the topology doesn't change throughout the run of a test, we can cache all the connection
// and re-use them to not overload the server with new connections.
const topologyCache = {};

function getTopologyConnections(conn) {
    if (!topologyCache.allConnections) {
        jsTest.log.debug(`Discovering topology...`);
        topologyCache.allConnections =
            getAllMongosConnections(conn)
                .flatMap(connection => DiscoverTopology.findNonConfigNodes(connection, {connectFn}))
                .map(host => connectFn(host));
    }
    return topologyCache.allConnections;
}

function getAllMongosConnections(conn) {
    if (!topologyCache.mongosConnectionsArr) {
        jsTest.log.debug(`Settings the mongos connections array...`);
        if (isMultiShardedClusterFixture) {
            const connections =
                conn.getDB("config").multiShardedClusterFixture.find().sort({_id: 1}).toArray();
            assert.eq(connections.length, 2);
            // Set the connections array to include both when using a multi-cluster fixture.
            topologyCache.mongosConnectionsArr =
                connections.map(doc => connectFn(doc.connectionString));
        } else {
            topologyCache.mongosConnectionsArr = [conn];
        }
    }
    return topologyCache.mongosConnectionsArr;
}

/**
 * Assert that all elements of 'array' are identical.
 */
function assertAllEqual(array) {
    assert(array.every(x => x === array[0]), `not all elements are same: ${array}`);
}

/**
 * Given a connection, discover all the cluster connected nodes (both mongod and mongos), and
 * assert that all the explain results for 'explainCmd' have identical query shape hashes.
 */
export function assertQueryShapeHashStability(conn, dbName, explainCmd) {
    try {
        // We run explain on all connections in the topology and assert that the query shape hash is
        // the same on all nodes.
        const queryShapeHashes =
            getTopologyConnections(conn).map(conn => conn.getDB(dbName)).map(db => {
                jsTest.log.info(`About to run the explain on host: ${db.getMongo().host}`);
                const explainResult = retryOnRetryableError(
                    () => assert.commandWorked(db.runCommand(explainCmd)), 50);
                return explainResult.queryShapeHash;
            });
        assertAllEqual(queryShapeHashes);
    } catch (ex) {
        const expectedErrorCodes =
            [ErrorCodes.CommandOnShardedViewNotSupportedOnMongod, ErrorCodes.NamespaceNotFound];
        if (!expectedErrorCodes.includes(ex.code)) {
            throw ex;
        }
    }
}

function runCommandOverride(conn, dbName, cmdName, cmdObj, clientFunction, makeFuncArgs) {
    // Do not run explain on queries that have 'batchSize' set to zero, as in majority of the tests
    // we are expecting an error when calling getMore() on that cursor.
    const hasBatchSizeZero = cmdObj.cursor && cmdObj.cursor.batchSize === 0;
    const res = clientFunction.apply(conn, makeFuncArgs(cmdObj));
    if (res.ok && !hasBatchSizeZero) {
        // Only run the test if the original command works. Some tests assert on commands failing,
        // so we should simply bubble these commands through without any additional checks.
        OverrideHelpers.withPreOverrideRunCommand(() => {
            if (isMultiShardedClusterFixture) {
                const mongosConnArr = getAllMongosConnections(conn);
                // In case we run the test using the two-cluster fixture, assert we have exactly two
                // mongos connections.
                assert.eq(mongosConnArr.length, 2);
                // Mirror the command on the second cluster to ensure the collections exists.
                // TODO SERVER-100658 Explain on non-existent collection returns empty results for
                // sharded cluster aggregations - Assess if this is still needed.
                const secondClusterMongos = mongosConnArr[1];
                retryOnRetryableError(
                    () => clientFunction.apply(secondClusterMongos, makeFuncArgs(cmdObj)), 50);
                FixtureHelpers.awaitReplication(secondClusterMongos.getDB("admin"));
            }
            const innerCmd = getInnerCommand(cmdObj);
            if (!QuerySettingsUtils.isSupportedCommand(getCommandName(innerCmd))) {
                return;
            }
            // Wrap command into explain, if it's not explain yet.
            const explainCmd = getExplainCommand(innerCmd);
            assertQueryShapeHashStability(conn, dbName, explainCmd);
        });
    }
    return res;
}

// Override the default runCommand with our custom version.
OverrideHelpers.overrideRunCommand(runCommandOverride);

// Always apply the override if a test spawns a parallel shell.
OverrideHelpers.prependOverrideInParallelShell(
    "jstests/libs/override_methods/query_shape_hash_stability.js");
