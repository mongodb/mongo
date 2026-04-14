/**
 * Tests that the max connections overrides are respected for connections via the priority port.
 *
 * Maximum connection overrides are not implemented for gRPC.
 * @tags: [
 *      # The priority port is based on ASIO, so gRPC testing is excluded
 *      grpc_incompatible,
 *      requires_sharding,
 *      requires_fcv_83,
 * ]
 */
import {MaxConnsOverrideHelpers} from "jstests/noPassthrough/libs/max_conns_override_helpers.js";
import {get_ipaddr} from "jstests/libs/network/host_ipaddr.js";
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
        priorityPort: allocatePort(),
    };

    const host = {
        admin: `${ip}:${opts.priorityPort}`,
        normal: `${ip}:${opts.port}`,
    };

    const proxyServer = {};

    jsTest.log(`Listening: ${opts.port}, ${opts.priorityPort}`);

    return {opts: opts, hosts: host, proxyServer: proxyServer};
}

/**
 * MongoRunner may connect as soon as the main port listens, but the priority listener can bind
 * later during startup. Wait until the priority address accepts connections before running the
 * test (see BF-42425).
 */
function waitForPriorityPortReady(hosts) {
    assert.soon(
        () => {
            try {
                const c = new Mongo(hosts.admin);
                c.close();
                return true;
            } catch (unusedErr) {
                return false;
            }
        },
        "Timed out waiting for priority port to accept connections",
        60000,
    );
}

function setupTestReplicaSet(useAdminThreads) {
    let args = getOptions(useAdminThreads);
    const m = MongoRunner.runMongod(args.opts);
    args.conn = m;
    args.shutdown = () => MongoRunner.stopMongod(m);
    waitForPriorityPortReady(args.hosts);
    return args;
}

function setupTestSharded(useAdminThreads) {
    let args = getOptions(useAdminThreads);
    const s = new ShardingTest({mongos: [args.opts], config: 1, shards: 1, useHostname: false});
    args.conn = s.s0;
    args.shutdown = () => s.stop();
    waitForPriorityPortReady(args.hosts);
    return args;
}

jsTest.log.info("Testing replica set with reserved admin threads");
{
    let args = setupTestReplicaSet(true);
    MaxConnsOverrideHelpers.runTest(
        args.conn,
        args.hosts,
        args.proxyServer,
        true /* usePriority */,
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
        true /* usePriority */,
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
        true /* usePriority */,
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
        true /* usePriority */,
        0 /* reservedAdminThreads */,
        kConfiguredMaxConns,
        args.shutdown,
    );
}
