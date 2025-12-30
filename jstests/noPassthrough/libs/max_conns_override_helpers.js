export var MaxConnsOverrideHelpers = (function () {
    // Get serverStatus to check that we have the right number of threads in the right places
    function getStats(conn) {
        return assert.commandWorked(conn.adminCommand({serverStatus: 1}));
    }

    function awaitServerStatusUpdated(conn, {exemptCount, normalCount, adminThreads}) {
        const totalCount = exemptCount + normalCount;
        // Verify that we have updated serverStatus.
        assert.soon(
            () => {
                const serverStatus = getStats(conn);
                const executors = serverStatus.network.serviceExecutors;

                const currentCount = serverStatus.connections.current;
                if (currentCount != totalCount) {
                    print(`Not yet at the expected count of connections: ${currentCount} != ${totalCount}`);
                    return false;
                }

                if (adminThreads > 0) {
                    const readyAdminThreads = executors.reserved.threadsRunning - executors.reserved.clientsRunning;
                    if (readyAdminThreads < adminThreads) {
                        print("Not enough admin threads yet: " + `${readyAdminThreads} < ${adminThreads}`);
                        return false;
                    }
                }

                const threadedCount = serverStatus.connections.threaded;
                const nonAdminClients = executors.passthrough.clientsInTotal;
                const adminClients = adminThreads > 0 ? executors.reserved.clientsInTotal : 0;
                const threadedExecutorCount = nonAdminClients + adminClients;
                if (threadedCount != threadedExecutorCount) {
                    print("Not enough running threaded clients yet: " + `${threadedCount} != ${threadedExecutorCount}`);
                    return false;
                }

                return true;
            },
            "Failed to verify initial conditions",
            10000,
        );
    }

    function verifyStats(conn, {exemptCount, normalCount, adminThreads, maxConns}) {
        const totalCount = exemptCount + normalCount;

        awaitServerStatusUpdated(conn, {exemptCount, normalCount, adminThreads});

        const serverStatus = getStats(conn);
        const connectionsStatus = serverStatus.connections;
        const reservedExecutorStatus = serverStatus.network.serviceExecutors.reserved;
        const executorStatus = serverStatus.network.serviceExecutors.passthrough;

        // Log these serverStatus sections so we can debug this easily.
        const filteredSections = {
            connections: connectionsStatus,
            network: {serviceExecutors: {passthrough: executorStatus, reserved: reservedExecutorStatus}},
        };
        print(`serverStatus: ${tojson(filteredSections)}`);

        if (totalCount > maxConns) {
            // If we're over maxConns, there are no available connections.
            assert.lte(connectionsStatus["available"], -1);
        } else {
            assert.eq(connectionsStatus["available"], maxConns - totalCount);
        }

        // All connections on an exempt CIDR should be marked as limitExempt.
        assert.eq(connectionsStatus["limitExempt"], exemptCount);

        // The normal serviceExecutor should only be running at most maxConns number of threads.
        if (adminThreads > 0) {
            assert.lte(executorStatus["threadsRunning"], maxConns);
        } else {
            assert.eq(executorStatus["clientsRunning"], executorStatus["threadsRunning"]);
        }

        // Clients on the normal executor own their thread and cannot wait asynchronously.
        assert.eq(executorStatus["clientsRunning"], executorStatus["clientsInTotal"]);
        assert.lte(executorStatus["clientsRunning"], executorStatus["threadsRunning"]);
        assert.eq(executorStatus["clientsWaitingForData"], 0);

        // Clients on the reserved executor run on a thread and cannot wait asynchronously.
        if (adminThreads > 0) {
            assert.eq(reservedExecutorStatus["clientsRunning"], reservedExecutorStatus["clientsInTotal"]);
            assert.lte(reservedExecutorStatus["clientsRunning"], reservedExecutorStatus["threadsRunning"]);
            assert.eq(reservedExecutorStatus["clientsWaitingForData"], 0);
        }
    }

    function runTest(conn, hosts, proxyServer, useMaintenance, adminThreads, maxConns, shutdown) {
        try {
            let adminConns = [];
            let normalConns = [];

            // In the non-maintenance port tests, the connections from test setup are exempt whereas
            // in the maintenance port tests those connections are normal connections.
            let exemptStartCount = useMaintenance ? 0 : 1;
            let normalStartCount = useMaintenance ? 1 : 0;
            let exemptCount = exemptStartCount;
            let normalCount = normalStartCount;

            // Do an initial verification.
            verifyStats(conn, {
                exemptCount: exemptCount,
                normalCount: normalCount,
                adminThreads: adminThreads,
                maxConns: maxConns,
            });

            for (let i = 0; i < 2 * maxConns; i++) {
                // Make some connections using the exempt CIDR and some using the normal CIDR.
                let isExempt = i % 2 == 0;
                try {
                    if (isExempt) {
                        adminConns.push(new Mongo(hosts.admin));
                        ++exemptCount;
                    } else {
                        normalConns.push(new Mongo(hosts.normal));
                        ++normalCount;
                    }
                } catch (e) {
                    jsTest.log("Threw exception: " + tojson(e));

                    // If we couldn't connect, that means we've exceeded maxConns
                    // and we're using the normal CIDR.
                    assert(!isExempt);
                    assert(i >= maxConns);
                }

                verifyStats(conn, {
                    exemptCount: exemptCount,
                    normalCount: normalCount,
                    adminThreads: adminThreads,
                    maxConns: maxConns,
                });
            }

            // Some common sense assertions around what was admitted.
            assert.eq(exemptCount, maxConns + exemptStartCount);
            assert.lte(normalCount, maxConns + normalStartCount);

            // Destroy all admin connections and verify assumptions.
            while (adminConns.length) {
                adminConns.pop().close();
                --exemptCount;

                verifyStats(conn, {
                    exemptCount: exemptCount,
                    normalCount: normalCount,
                    adminThreads: adminThreads,
                    maxConns: maxConns,
                });
            }

            // Destroy all normal connections and verify assumptions.
            while (normalConns.length) {
                normalConns.pop().close();
                --normalCount;

                verifyStats(conn, {
                    exemptCount: exemptCount,
                    normalCount: normalCount,
                    adminThreads: adminThreads,
                    maxConns: maxConns,
                });
            }
        } finally {
            shutdown();
            Object.values(proxyServer).forEach((p) => p.stop());
        }
    }

    return {runTest: runTest};
})();
