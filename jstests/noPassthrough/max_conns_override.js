(function() {
    'use strict';
    const configuredMaxConns = 5;
    const configuredReadyAdminThreads = 3;
    let conn = MongoRunner.runMongod({
        config: "jstests/noPassthrough/libs/max_conns_override_config.yaml",
        // We check a specific field in this executor's serverStatus section
        serviceExecutor: "synchronous",
    });

    // Use up all the maxConns with junk connections, all of these should succeed
    let maxConns = [];
    for (let i = 0; i < 5; i++) {
        maxConns.push(new Mongo(`127.0.0.1:${conn.port}`));
        let tmpDb = maxConns[maxConns.length - 1].getDB("admin");
        assert.commandWorked(tmpDb.runCommand({isMaster: 1}));
    }

    // Get serverStatus to check that we have the right number of threads in the right places
    let status = conn.getDB("admin").runCommand({serverStatus: 1});
    const connectionsStatus = status["connections"];
    const reservedExecutorStatus = connectionsStatus["adminConnections"];
    const normalExecutorStatus = status["network"]["serviceExecutorTaskStats"];

    // Log these serverStatus sections so we can debug this easily
    print("connections status section: ", tojson(connectionsStatus));
    print("normal executor status section: ", tojson(normalExecutorStatus));

    // The number of "available" connections should be less than zero, because we've used
    // all of maxConns. We're over the limit!
    assert.lt(connectionsStatus["available"], 0);
    // The number of "current" connections should be greater than maxConns
    assert.gt(connectionsStatus["current"], configuredMaxConns);
    // The number of ready threads should be the number of readyThreads we configured, since
    // every thread spawns a new thread on startup
    assert.eq(reservedExecutorStatus["readyThreads"] + reservedExecutorStatus["startingThreads"],
              configuredReadyAdminThreads);
    // The number of running admin threads should be greater than the readyThreads, because
    // one is being used right now
    assert.gt(reservedExecutorStatus["threadsRunning"], reservedExecutorStatus["readyThreads"]);
    // The normal serviceExecutor should only be running maxConns number of threads
    assert.eq(normalExecutorStatus["threadsRunning"], configuredMaxConns);

    MongoRunner.stopMongod(conn);
})();
