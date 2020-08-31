(function() {
'use strict';

load("jstests/libs/host_ipaddr.js");

const configuredMaxConns = 5;
const configuredReadyAdminThreads = 3;
let conn = MongoRunner.runMongod({
    config: "jstests/noPassthrough/libs/max_conns_override_config.yaml",
});

// Get serverStatus to check that we have the right number of threads in the right places
function getStats() {
    return assert.commandWorked(conn.getDB("admin").runCommand({serverStatus: 1}));
}

function verifyStats({exemptCount, normalCount}) {
    const totalCount = exemptCount + normalCount;

    // Verify that we have updated serverStatus.
    assert.soon(() => {
        const serverStatus = getStats();

        const readyAdminThreads = serverStatus.connections.adminConnections.readyThreads;
        if (readyAdminThreads < configuredReadyAdminThreads) {
            print(`Not enough admin threads yet: ${readyAdminThreads} vs ${
                configuredReadyAdminThreads}`);
            return false;
        }

        const currentCount = serverStatus.connections.current;
        if (currentCount != totalCount) {
            print(`Not yet at the expected count of connections: ${currentCount} vs ${totalCount}`);
            return false;
        }

        return true;
    }, "Failed to verify initial conditions", 10000);

    const serverStatus = getStats();
    const connectionsStatus = serverStatus.connections;
    const reservedExecutorStatus = connectionsStatus.adminConnections;
    const executorStatus = serverStatus.network.serviceExecutorTaskStats;

    // Log these serverStatus sections so we can debug this easily.
    const filteredSections = {
        connections: connectionsStatus,
        network: {
            serviceExecutorTaskStats: executorStatus,
        }
    };
    print(`serverStatus: ${tojson(filteredSections)}`);

    if (totalCount > configuredMaxConns) {
        // If we're over maxConns, there are no available connections.
        assert.lte(connectionsStatus["available"], -1);
    } else {
        assert.eq(connectionsStatus["available"], configuredMaxConns - totalCount);
    }

    // All connections on an exempt CIDR should be marked as limitExempt.
    assert.eq(connectionsStatus["limitExempt"], exemptCount);

    // Without a borrowing executor, all connections are threaded.
    assert.eq(connectionsStatus["threaded"], totalCount);

    if (totalCount > configuredMaxConns) {
        // The normal serviceExecutor should only be running at most maxConns number of threads.
        assert.eq(executorStatus["threadsRunning"], configuredMaxConns);
    } else {
        assert.eq(executorStatus["threadsRunning"], totalCount);
    }

    // We should have all excess connections on the reserved executor.
    assert.gt(reservedExecutorStatus["threadsRunning"], totalCount - configuredMaxConns);
}

// Use the external ip to avoid our exempt CIDR.
let ip = get_ipaddr();

try {
    let adminConns = [];
    let normalConns = [];

    // We start with one exempt control socket.
    let exemptCount = 1;
    let normalCount = 0;

    // Do an initial verification.
    verifyStats({exemptCount: exemptCount, normalCount: normalCount});

    for (let i = 0; i < 2 * configuredMaxConns; i++) {
        // Make some connections using the exempt CIDR and some using the normal CIDR.
        let isExempt = (i % 2 == 0);
        try {
            if (isExempt) {
                adminConns.push(new Mongo(`127.0.0.1:${conn.port}`));
                ++exemptCount;
            } else {
                normalConns.push(new Mongo(`${ip}:${conn.port}`));
                ++normalCount;
            }
        } catch (e) {
            print(e);

            // If we couldn't connect, that means we've exceeded maxConns and we're using the normal
            // CIDR.
            assert(!isExempt);
            assert(i >= configuredMaxConns);
        }

        verifyStats({exemptCount: exemptCount, normalCount: normalCount});
    }

    // Some common sense assertions around what was admitted.
    assert.eq(exemptCount, configuredMaxConns + 1);
    assert.lte(normalCount, configuredMaxConns);

    // Destroy all admin connections and verify assumptions.
    while (adminConns.length) {
        adminConns.pop().close();
        --exemptCount;

        verifyStats({exemptCount: exemptCount, normalCount: normalCount});
    }

    // Destroy all normal connections and verify assumptions.
    while (normalConns.length) {
        normalConns.pop().close();
        --normalCount;

        verifyStats({exemptCount: exemptCount, normalCount: normalCount});
    }
} finally {
    MongoRunner.stopMongod(conn);
}
})();
