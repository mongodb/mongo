/**
 * Tests that the max connections overrides are respected for connections via the maintenance port.
 *
 * Maximum connection overrides are not implemented for gRPC.
 * @tags: [
 *      grpc_incompatible,
 *      requires_sharding,
 *      featureFlagDedicatedPortForMaintenanceOperations,
 * ]
 */
import {MaxConnsOverrideHelpers} from "jstests/noPassthrough/libs/max_conns_override_helpers.js";
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const kConfiguredMaxConns = 5;
const kConfiguredReadyAdminThreads = 3;

function getOptions(useAdminThreads) {
    const ip = get_ipaddr();
    const opts = {
        config: useAdminThreads
            ? "jstests/noPassthrough/libs/max_conns_config.yaml"
            : "jstests/noPassthrough/libs/max_conns_config_no_admin_threads.yaml",
        port: allocatePort(),
        maintenancePort: allocatePort(),
    };

    const host = {
        admin: `${ip}:${opts.maintenancePort}`,
        normal: `${ip}:${opts.port}`,
    };

    const proxyServer = {};

    jsTest.log(`Listening: ${opts.port}, ${opts.maintenancePort}`);

    return {opts: opts, hosts: host, proxyServer: proxyServer};
}

function setupTestReplicaSet(useAdminThreads) {
    let args = getOptions(useAdminThreads);
    const m = MongoRunner.runMongod(args.opts);
    args.conn = m;
    args.shutdown = () => MongoRunner.stopMongod(m);
    return args;
}

function setupTestSharded(useAdminThreads) {
    let args = getOptions(useAdminThreads);
    const s = new ShardingTest({mongos: [args.opts], config: 1, shards: 1, useHostname: false});
    args.conn = s.s0;
    args.shutdown = () => s.stop();
    return args;
}

jsTest.log.info("Testing replica set with reserved admin threads");
{
    let args = setupTestReplicaSet(true);
    MaxConnsOverrideHelpers.runTest(
        args.conn,
        args.hosts,
        args.proxyServer,
        true /* useMaintenance */,
        kConfiguredReadyAdminThreads,
        kConfiguredMaxConns,
        args.shutdown,
    );
}

jsTest.log.info("Testing replica set without reserved admin threads");
{
    let args = setupTestReplicaSet(false);
    MaxConnsOverrideHelpers.runTest(
        args.conn,
        args.hosts,
        args.proxyServer,
        true /* useMaintenance */,
        0 /* reservedAdminThreads */,
        kConfiguredMaxConns,
        args.shutdown,
    );
}

jsTest.log.info("Testing sharded cluster with reserved admin threads");
{
    let args = setupTestSharded(true);
    MaxConnsOverrideHelpers.runTest(
        args.conn,
        args.hosts,
        args.proxyServer,
        true /* useMaintenance */,
        kConfiguredReadyAdminThreads,
        kConfiguredMaxConns,
        args.shutdown,
    );
}

jsTest.log.info("Testing sharded cluster without reserved admin threads");
{
    let args = setupTestSharded(false);
    MaxConnsOverrideHelpers.runTest(
        args.conn,
        args.hosts,
        args.proxyServer,
        true /* useMaintenance */,
        0 /* reservedAdminThreads */,
        kConfiguredMaxConns,
        args.shutdown,
    );
}
