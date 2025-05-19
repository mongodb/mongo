/**
 * Tests that the max connections overrides are respected for exempt IPs.
 *
 * Maximum connection overrides are not implemented for gRPC.
 * @tags: [
 *      grpc_incompatible,
 *      requires_sharding,
 * ]
 */
load("jstests/sharding/libs/proxy_protocol.js");

(function() {
'use strict';

load("jstests/libs/host_ipaddr.js");
load("jstests/libs/feature_flag_util.js");

const kConfiguredMaxConns = 5;
const kConfiguredReadyAdminThreads = 3;

// Get serverStatus to check that we have the right number of threads in the right places
function getStats(conn) {
    return assert.commandWorked(conn.adminCommand({serverStatus: 1}));
}

function verifyStats(conn, {exemptCount, normalCount}) {
    const totalCount = exemptCount + normalCount;

    // Verify that we have updated serverStatus.
    assert.soon(() => {
        const serverStatus = getStats(conn);
        const executors = serverStatus.network.serviceExecutors;

        const currentCount = serverStatus.connections.current;
        if (currentCount != totalCount) {
            print(`Not yet at the expected count of connections: ${currentCount} != ${totalCount}`);
            return false;
        }

        const readyAdminThreads =
            executors.reserved.threadsRunning - executors.reserved.clientsRunning;
        if (readyAdminThreads < kConfiguredReadyAdminThreads) {
            print("Not enough admin threads yet: " +
                  `${readyAdminThreads} < ${kConfiguredReadyAdminThreads}`);
            return false;
        }

        const threadedCount = serverStatus.connections.threaded;
        const threadedExecutorCount =
            executors.passthrough.clientsInTotal + executors.reserved.clientsInTotal;
        if (threadedCount != threadedExecutorCount) {
            print("Not enough running threaded clients yet: " +
                  `${threadedCount} != ${threadedExecutorCount}`);
            return false;
        }

        const totalExecutorCount = threadedExecutorCount + executors.fixed.clientsInTotal;
        if (totalCount != totalExecutorCount) {
            print(`Not enough running clients yet: ${totalCount} != ${totalExecutorCount}`);
            return false;
        }

        return true;
    }, "Failed to verify initial conditions", 10000);

    const serverStatus = getStats(conn);
    const connectionsStatus = serverStatus.connections;
    const reservedExecutorStatus = serverStatus.network.serviceExecutors.reserved;
    const fixedExecutorStatus = serverStatus.network.serviceExecutors.fixed;
    const executorStatus = serverStatus.network.serviceExecutors.passthrough;

    // Log these serverStatus sections so we can debug this easily.
    const filteredSections = {
        connections: connectionsStatus,
        network: {
            serviceExecutors: {
                passthrough: executorStatus,
                fixed: fixedExecutorStatus,
                reserved: reservedExecutorStatus
            }
        }
    };
    print(`serverStatus: ${tojson(filteredSections)}`);

    if (totalCount > kConfiguredMaxConns) {
        // If we're over maxConns, there are no available connections.
        assert.lte(connectionsStatus["available"], -1);
    } else {
        assert.eq(connectionsStatus["available"], kConfiguredMaxConns - totalCount);
    }

    // All connections on an exempt CIDR should be marked as limitExempt.
    assert.eq(connectionsStatus["limitExempt"], exemptCount);

    // The normal serviceExecutor should only be running at most maxConns number of threads.
    assert.lte(executorStatus["threadsRunning"], kConfiguredMaxConns);

    // Clients on the normal executor own their thread and cannot wait asynchronously.
    assert.eq(executorStatus["clientsRunning"], executorStatus["clientsInTotal"]);
    assert.lte(executorStatus["clientsRunning"], executorStatus["threadsRunning"]);
    assert.eq(executorStatus["clientsWaitingForData"], 0);

    // Clients on the reserved executor run on a thread and cannot wait asynchronously.
    assert.eq(reservedExecutorStatus["clientsRunning"], reservedExecutorStatus["clientsInTotal"]);
    assert.lte(reservedExecutorStatus["clientsRunning"], reservedExecutorStatus["threadsRunning"]);
    assert.eq(reservedExecutorStatus["clientsWaitingForData"], 0);

    // Clients on the fixed executor borrow one thread and can wait asynchronously
    assert.lte(fixedExecutorStatus["clientsRunning"], fixedExecutorStatus["clientsInTotal"]);
    assert.lte(fixedExecutorStatus["clientsRunning"], fixedExecutorStatus["threadsRunning"]);
    assert.lte(fixedExecutorStatus["clientsWaitingForData"], fixedExecutorStatus["clientsInTotal"]);
}

// Can't run the proxy on Windows.
// If we're on POSIX, then determine doing the proxy test based on checking FF enable.
// (deferred until first run without proxy)
let featureFlagMongodProxyProcolSupportEnabled = _isWindows() ? false : undefined;

function runTest(useProxy, useMongos) {
    jsTest.log(`runTest(useProxy: ${tojson(useProxy)}, useMongos: ${tojson(useMongos)})`);

    // Use the external ip to avoid our exempt CIDR.
    const ip = get_ipaddr();
    const opts = {
        config: "jstests/noPassthrough/libs/max_conns_override_config.yaml",
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
            admin: '127.0.0.1',
            normal: ip,
        };

        Object.keys(servers).forEach(function(mode) {
            const svr =
                new ProxyProtocolServer(allocatePort(), opts.proxyPort, kProxyProtocolVersion);
            svr.ingress_address = servers[mode];
            svr.egress_address = servers[mode];
            svr.start();
            host[mode] = svr.getIngressString();
            proxyServer[mode] = svr;
        });

        jsTest.log(`ProxyPort: ${opts.proxyPort}`);
        Object.keys(proxyServer).forEach(function(mode) {
            const svr = proxyServer[mode];
            jsTest.log(
                `ProxyServer ${mode}: ${svr.getIngressString()} -> ${svr.getEgressString()}`);
        });
    }
    jsTest.log(`Listening: ${opts.port}`);

    const {conn, shutdown} = function() {
        if (useMongos) {
            if (useProxy) {
                // mongos accepts proxied connections on loadBalancerPort, not proxyPort. Rename.
                opts.setParameter = opts.setParameter || {};
                opts.setParameter.loadBalancerPort = opts.proxyPort;
                delete opts.proxyPort;

                // Client must connect with "?loadBalanced=true" in URI when proxying to mongos.
                Object.keys(host).forEach(function(key) {
                    host[key] = `mongodb://${host[key]}/admin?loadBalanced=true`;
                });
            }
            const s = new ShardingTest({mongos: [opts], config: 1, shards: 1, useHostname: false});
            return {conn: s.s0, shutdown: () => s.stop()};
        } else {
            const m = MongoRunner.runMongod(opts);
            return {conn: m, shutdown: () => MongoRunner.stopMongod(m)};
        }
    }();

    try {
        if (featureFlagMongodProxyProcolSupportEnabled === undefined) {
            // Test for featureFlag while in normal mode,
            // so that we know if we can run with proxy mode later.
            featureFlagMongodProxyProcolSupportEnabled =
                FeatureFlagUtil.isEnabled(conn, 'MongodProxyProcolSupport');
        }

        let adminConns = [];
        let normalConns = [];

        // We start with one exempt control socket.
        let exemptCount = 1;
        let normalCount = 0;

        // Do an initial verification.
        verifyStats(conn, {exemptCount: exemptCount, normalCount: normalCount});

        for (let i = 0; i < 2 * kConfiguredMaxConns; i++) {
            // Make some connections using the exempt CIDR and some using the normal CIDR.
            let isExempt = (i % 2 == 0);
            try {
                if (isExempt) {
                    adminConns.push(new Mongo(host.admin));
                    ++exemptCount;
                } else {
                    normalConns.push(new Mongo(host.normal));
                    ++normalCount;
                }
            } catch (e) {
                jsTest.log('Threw exception: ' + tojson(e));

                // If we couldn't connect, that means we've exceeded maxConns
                // and we're using the normal CIDR.
                assert(!isExempt);
                assert(i >= kConfiguredMaxConns);
            }

            verifyStats(conn, {exemptCount: exemptCount, normalCount: normalCount});
        }

        // Some common sense assertions around what was admitted.
        assert.eq(exemptCount, kConfiguredMaxConns + 1);
        assert.lte(normalCount, kConfiguredMaxConns);

        // Destroy all admin connections and verify assumptions.
        while (adminConns.length) {
            adminConns.pop().close();
            --exemptCount;

            verifyStats(conn, {exemptCount: exemptCount, normalCount: normalCount});
        }

        // Destroy all normal connections and verify assumptions.
        while (normalConns.length) {
            normalConns.pop().close();
            --normalCount;

            verifyStats(conn, {exemptCount: exemptCount, normalCount: normalCount});
        }
    } finally {
        shutdown();
        Object.values(proxyServer).forEach((p) => p.stop());
    }
}

function runTestSet(useProxy) {
    runTest(useProxy, false /* mongos */);
    runTest(useProxy, true /* mongos */);
}

// Ordering of false->true requied for identifying if we have ff enabled.
runTestSet(false);
if (featureFlagMongodProxyProcolSupportEnabled) {
    runTestSet(true);
}
})();
