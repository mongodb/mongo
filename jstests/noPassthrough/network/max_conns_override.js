/**
 * Tests that the max connections overrides are respected for exempt IPs.
 *
 * Maximum connection overrides are not implemented for gRPC.
 * @tags: [
 *      grpc_incompatible,
 *      requires_sharding,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {get_ipaddr} from "jstests/libs/host_ipaddr.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ProxyProtocolServer} from "jstests/sharding/libs/proxy_protocol.js";
import {MaxConnsOverrideHelpers} from "jstests/noPassthrough/libs/max_conns_override_helpers.js";

const kConfiguredMaxConns = 5;
const kConfiguredReadyAdminThreads = 3;

// Can't run the proxy on Windows.
// If we're on POSIX, then determine doing the proxy test based on checking FF enable.
// (deferred until first run without proxy)
let featureFlagMongodProxyProtocolSupportEnabled = _isWindows() ? false : undefined;

function runTest(useProxy, useMongos, useAdminThreads) {
    jsTest.log(
        `runTest(useProxy: ${tojson(useProxy)}, useMongos: ${tojson(useMongos)}, useAdminThreads: ${tojson(useAdminThreads)})`,
    );

    // Use the external ip to avoid our exempt CIDR.
    const ip = get_ipaddr();
    const opts = {
        config: useAdminThreads
            ? "jstests/noPassthrough/libs/max_conns_override_config.yaml"
            : "jstests/noPassthrough/libs/max_conns_override_config_no_admin_threads.yaml",
        port: allocatePort(),
    };

    const host = {
        admin: `127.0.0.1:${opts.port}`,
        normal: `${ip}:${opts.port}`,
    };

    const proxyServer = {};
    if (useProxy) {
        const kProxyProtocolVersion = 2;
        opts.proxyPort = allocatePort();

        // Start up two proxy servers, one on localhost, the other on the public address.
        const servers = {
            admin: "127.0.0.1",
            normal: ip,
        };

        Object.keys(servers).forEach(function (mode) {
            const svr = new ProxyProtocolServer(allocatePort(), opts.proxyPort, kProxyProtocolVersion);
            svr.ingress_address = servers[mode];
            svr.egress_address = servers[mode];
            svr.start();
            host[mode] = svr.getIngressString();
            proxyServer[mode] = svr;
        });

        jsTest.log(`ProxyPort: ${opts.proxyPort}`);
        Object.keys(proxyServer).forEach(function (mode) {
            const svr = proxyServer[mode];
            jsTest.log(`ProxyServer ${mode}: ${svr.getIngressString()} -> ${svr.getEgressString()}`);
        });
    }
    jsTest.log(`Listening: ${opts.port}`);

    const {conn, shutdown} = (function () {
        if (useMongos) {
            if (useProxy) {
                // mongos accepts proxied connections on loadBalancerPort, not proxyPort. Rename.
                opts.setParameter = opts.setParameter || {};
                opts.setParameter.loadBalancerPort = opts.proxyPort;
                delete opts.proxyPort;

                // Client must connect with "?loadBalanced=true" in URI when proxying to mongos.
                Object.keys(host).forEach(function (key) {
                    host[key] = `mongodb://${host[key]}/admin?loadBalanced=true`;
                });
            }
            const s = new ShardingTest({mongos: [opts], config: 1, shards: 1, useHostname: false});
            return {conn: s.s0, shutdown: () => s.stop()};
        } else {
            const m = MongoRunner.runMongod(opts);
            return {conn: m, shutdown: () => MongoRunner.stopMongod(m)};
        }
    })();

    if (featureFlagMongodProxyProtocolSupportEnabled === undefined) {
        // Test for featureFlag while in normal mode,
        // so that we know if we can run with proxy mode later.
        featureFlagMongodProxyProtocolSupportEnabled = FeatureFlagUtil.isEnabled(conn, "MongodProxyProtocolSupport");
    }

    MaxConnsOverrideHelpers.runTest(
        conn,
        host,
        proxyServer,
        false /* useMaintenance */,
        useAdminThreads ? kConfiguredReadyAdminThreads : 0,
        kConfiguredMaxConns,
        shutdown,
    );
}

function runTestSet(useProxy) {
    runTest(useProxy, false /* mongos */, true /* useAdminThreads */);
    runTest(useProxy, true /* mongos */, true /* useAdminThreads */);
    runTest(useProxy, false /* mongos */, false /* useAdminThreads */);
    runTest(useProxy, true /* mongos */, false /* useAdminThreads */);
}

// Ordering of false->true required for identifying if we have ff enabled.
runTestSet(false);
if (featureFlagMongodProxyProtocolSupportEnabled) {
    runTestSet(true);
}
