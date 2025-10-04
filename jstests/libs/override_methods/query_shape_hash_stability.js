/**
 * Overrides Mongo.prototype.runCommand for query settings supported commands in order to run
 * explain on them multiple times in a row and ensure that the reported 'queryShapeHash' value is
 * same.
 **/
import {getCommandName, getExplainCommand, getInnerCommand} from "jstests/libs/cmd_object_utils.js";
import {DiscoverTopology} from "jstests/libs/discover_topology.js";
import {FixtureHelpers} from "jstests/libs/fixture_helpers.js";
import {getCollectionNameFromFullNamespace} from "jstests/libs/namespace_utils.js";
import {OverrideHelpers} from "jstests/libs/override_methods/override_helpers.js";
import {getQueryPlanners} from "jstests/libs/query/analyze_plan.js";
import {QuerySettingsUtils} from "jstests/libs/query/query_settings_utils.js";

/**
 * We need to set secondary ok in order to propagate commands on secondary nodes.
 */
const connectFn = (host) => {
    const conn = new Mongo(host, undefined, {gRPC: false});
    conn.setSecondaryOk();
    return conn;
};

// Flag which tracks if we run this test using the two-cluster fixture.
const isMultiShardedClusterFixture = TestData.isMultiShardedClusterFixture || false;

// Because the topology doesn't change throughout the run of a test, we can cache all the connection
// and re-use them to not overload the server with new connections.
const topologyCache = {};

function getTopologyConnections(conn) {
    if (!topologyCache.allConnections) {
        jsTest.log.debug(`Discovering topology...`);
        topologyCache.allConnections = getAllMongosConnections(conn)
            .flatMap((connection) => DiscoverTopology.findNonConfigNodes(connection))
            .map(connectFn);
        // Assert that all hosts are different.
        const setOfHosts = new Set(topologyCache.allConnections.map((el) => el.toString()));
        assert.eq(setOfHosts.size, topologyCache.allConnections.length);
        jsTest.log.debug("List vs set topology...", {setOfHosts, list: topologyCache.allConnections});
    }
    return topologyCache.allConnections;
}

function getAllMongosConnections(conn) {
    if (!topologyCache.mongosConnectionsArr) {
        jsTest.log.debug(`Settings the mongos connections array...`);
        if (isMultiShardedClusterFixture) {
            const connections = conn.getDB("config").multiShardedClusterFixture.find().sort({_id: 1}).toArray();
            assert.eq(connections.length, 2);
            // Set the connections array to include both when using a multi-cluster fixture.
            topologyCache.mongosConnectionsArr = connections.map((doc) => connectFn(doc.connectionString));
        } else {
            topologyCache.mongosConnectionsArr = [conn];
        }
    }
    return topologyCache.mongosConnectionsArr;
}

/**
 * Given a connection, discover all the cluster connected nodes (both mongod and mongos), and
 * assert that all the explain results for 'explainCmd' have identical query shape hashes.
 */
export function assertQueryShapeHashStability(conn, dbName, explainCmd) {
    let explainResults;
    try {
        // We run explain on all connections in the topology and assert that the query shape hash is
        // the same on all nodes.
        explainResults = getTopologyConnections(conn)
            .map((conn) => conn.getDB(dbName))
            .map((db) => {
                jsTest.log.info("About to run the explain", {host: db.getMongo().host});
                const explainResult = retryOnRetryableError(() => assert.commandWorked(db.runCommand(explainCmd)), 50);
                return explainResult;
            });
    } catch (ex) {
        // Fuzzer may generate invalid commands, which will fail on assert.commandWorked().
        // If explain command failed, ignore the exception.
        if (TestData.isRunningQueryShapeHashFuzzer) {
            return;
        }

        const expectedErrorCodes = [ErrorCodes.CommandOnShardedViewNotSupportedOnMongod, ErrorCodes.NamespaceNotFound];
        if (expectedErrorCodes.includes(ex.code)) {
            return;
        }

        throw ex;
    }

    const isRawOperationOnLegacyTimeseries = (() => {
        if (explainCmd.explain.rawData !== true) {
            // This is not a 'rawData' operation.
            return false;
        }
        const isSystemBucketsNamespace = (nss) => {
            return getCollectionNameFromFullNamespace(nss).startsWith("system.buckets.");
        };
        return explainResults.some((explainRes) =>
            getQueryPlanners(explainRes).some((queryPlanner) => isSystemBucketsNamespace(queryPlanner.namespace)),
        );
    })();

    // TODO SERVER-103551 remove this once query shape hash calculation for legacy timeseries
    // collection is fixed
    if (isRawOperationOnLegacyTimeseries) {
        // Operations that specify `rawData` targeting legacy timeseries collection will not produce
        // a query shape hash on the shards of a sharded cluster (SERVER-103069)
        return;
    }

    // Check that all the explain commands executed on all nodes returned the same 'queryShapeHash'.
    assert.gt(explainResults.length, 0, `Found explain results array to be empty`);
    const firstQueryShapeHash = explainResults[0].queryShapeHash;
    assert(
        explainResults.every((explainRes) => explainRes.queryShapeHash === firstQueryShapeHash),
        `Not all nodes returned same QueryShapeHash in explain command results. Explain command: ${tojson(
            explainCmd,
        )}. Explain results from all nodes: ${tojson(explainResults)}`,
    );
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
                retryOnRetryableError(() => clientFunction.apply(secondClusterMongos, makeFuncArgs(cmdObj)), 50);
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
OverrideHelpers.prependOverrideInParallelShell("jstests/libs/override_methods/query_shape_hash_stability.js");
