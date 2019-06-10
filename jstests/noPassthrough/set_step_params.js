load("jstests/libs/parallelTester.js");

/**
 * @tags: [requires_replication, requires_sharding]
 */

(function() {
    "use strict";

    const kDbName = 'test';

    const minConns = 4;
    var stepParams = {
        ShardingTaskExecutorPoolMinSize: minConns,
        ShardingTaskExecutorPoolMaxSize: 10,
        ShardingTaskExecutorPoolMaxConnecting: 5,
        ShardingTaskExecutorPoolHostTimeoutMS: 300000,
        ShardingTaskExecutorPoolRefreshRequirementMS: 60000,
        ShardingTaskExecutorPoolRefreshTimeoutMS: 20000,
        ShardingTaskExecutorPoolReplicaSetMatching: "disabled",
    };

    const st = new ShardingTest({
        config: {nodes: 1},
        shards: 1,
        rs0: {nodes: 1},
        mongos: [{setParameter: stepParams}],
    });
    const mongos = st.s0;
    const rst = st.rs0;
    const primary = rst.getPrimary();

    const cfg = primary.getDB('local').system.replset.findOne();
    const allHosts = cfg.members.map(x => x.host);
    const mongosDB = mongos.getDB(kDbName);
    const primaryOnly = [primary.name];

    function configureReplSetFailpoint(name, modeValue) {
        st.rs0.nodes.forEach(function(node) {
            assert.commandWorked(node.getDB("admin").runCommand({
                configureFailPoint: name,
                mode: modeValue,
                data: {shouldCheckForInterrupt: true},
            }));
        });
    }

    var threads = [];
    function launchFinds({times, readPref, shouldFail}) {
        jsTestLog("Starting " + times + " connections");
        for (var i = 0; i < times; i++) {
            var thread = new Thread(function(connStr, readPref, dbName, shouldFail) {
                var client = new Mongo(connStr);
                const ret = client.getDB(dbName).runCommand(
                    {find: "test", limit: 1, "$readPreference": {mode: readPref}});

                if (shouldFail) {
                    assert.commandFailed(ret);
                } else {
                    assert.commandWorked(ret);
                }
            }, st.s.host, readPref, kDbName, shouldFail);
            thread.start();
            threads.push(thread);
        }
    }

    var currentCheckNum = 0;
    function hasConnPoolStats(args) {
        const checkNum = currentCheckNum++;
        jsTestLog("Check #" + checkNum + ": " + tojson(args));
        var {ready, pending, active, hosts, isAbsent} = args;

        ready = ready ? ready : 0;
        pending = pending ? pending : 0;
        active = active ? active : 0;
        hosts = hosts ? hosts : allHosts;

        function checkStats(res, host) {
            var stats = res.hosts[host];
            if (!stats) {
                jsTestLog("Connection stats for " + host + " are absent");
                return isAbsent;
            }

            jsTestLog("Connection stats for " + host + ": " + tojson(stats));
            return stats.available == ready && stats.refreshing == pending && stats.inUse == active;
        }

        function checkAllStats() {
            var res = mongos.adminCommand({connPoolStats: 1});
            return hosts.map(host => checkStats(res, host)).every(x => x);
        }

        assert.soon(checkAllStats, "Check #" + checkNum + " failed", 10000);

        jsTestLog("Check #" + checkNum + " successful");
    }

    function updateSetParameters(params) {
        var cmd = Object.assign({"setParameter": 1}, params);
        assert.commandWorked(mongos.adminCommand(cmd));
    }

    function dropConnections() {
        assert.commandWorked(mongos.adminCommand({dropConnections: 1, hostAndPort: allHosts}));
    }

    function resetPools() {
        dropConnections();
        mongos.adminCommand({multicast: {ping: 0}});
        hasConnPoolStats({ready: 4});
        dropConnections();
        hasConnPoolStats({});
    }

    function runSubTest(name, fun) {
        jsTestLog("Running test for " + name);

        resetPools();

        fun();

        updateSetParameters(stepParams);
    }

    assert.writeOK(mongosDB.test.insert({x: 1}));
    assert.writeOK(mongosDB.test.insert({x: 2}));
    assert.writeOK(mongosDB.test.insert({x: 3}));
    st.rs0.awaitReplication();

    runSubTest("MinSize", function() {
        // Launch an initial find to trigger to min
        launchFinds({times: 1, readPref: "primary"});
        hasConnPoolStats({ready: minConns});

        // Increase by one
        updateSetParameters({ShardingTaskExecutorPoolMinSize: 5});
        hasConnPoolStats({ready: 5});

        // Increase to MaxSize
        updateSetParameters({ShardingTaskExecutorPoolMinSize: 10});
        hasConnPoolStats({ready: 10});

        // Decrease to zero
        updateSetParameters({ShardingTaskExecutorPoolMinSize: 0});
    });

    runSubTest("MaxSize", function() {
        configureReplSetFailpoint("waitInFindBeforeMakingBatch", "alwaysOn");

        // Launch 10 blocked finds
        launchFinds({times: 10, readPref: "primary"});
        hasConnPoolStats({active: 10, hosts: primaryOnly});

        // Increase by 5 and Launch another 4 blocked finds
        updateSetParameters({ShardingTaskExecutorPoolMaxSize: 15});
        launchFinds({times: 4, readPref: "primary"});
        hasConnPoolStats({active: 14, hosts: primaryOnly});

        // Launch yet another 2, these should add only 1 connection
        launchFinds({times: 2, readPref: "primary"});
        hasConnPoolStats({active: 15, hosts: primaryOnly});

        configureReplSetFailpoint("waitInFindBeforeMakingBatch", "off");
        hasConnPoolStats({ready: 15, pending: 0, hosts: primaryOnly});
    });

    // Test maxConnecting
    runSubTest("MaxConnecting", function() {
        const maxPending1 = 2;
        const maxPending2 = 4;
        const conns = 6;

        updateSetParameters({
            ShardingTaskExecutorPoolMaxSize: 100,
            ShardingTaskExecutorPoolMaxConnecting: maxPending1,
        });

        configureReplSetFailpoint("waitInIsMaster", "alwaysOn");
        configureReplSetFailpoint("waitInFindBeforeMakingBatch", "alwaysOn");

        // Go to the limit of maxConnecting, so we're stuck here
        launchFinds({times: maxPending1, readPref: "primary"});
        hasConnPoolStats({pending: maxPending1});

        // More won't run right now
        launchFinds({times: conns - maxPending1, readPref: "primary"});
        hasConnPoolStats({pending: maxPending1});

        // If we increase our limit, it should fill in some of the connections
        updateSetParameters({ShardingTaskExecutorPoolMaxConnecting: maxPending2});
        hasConnPoolStats({pending: maxPending2});

        // Dropping the limit doesn't cause us to drop pending
        updateSetParameters({ShardingTaskExecutorPoolMaxConnecting: maxPending1});
        hasConnPoolStats({pending: maxPending2});

        // Release our pending and walk away
        configureReplSetFailpoint("waitInIsMaster", "off");
        hasConnPoolStats({active: conns});
        configureReplSetFailpoint("waitInFindBeforeMakingBatch", "off");
    });

    runSubTest("Timeouts", function() {
        const conns = minConns;
        const pendingTimeoutMS = 5000;
        const toRefreshTimeoutMS = 1000;
        const idleTimeoutMS1 = 20000;
        const idleTimeoutMS2 = 15500;

        // Updating separately since the validation depends on existing params
        updateSetParameters({
            ShardingTaskExecutorPoolRefreshTimeoutMS: pendingTimeoutMS,
        });
        updateSetParameters({
            ShardingTaskExecutorPoolRefreshRequirementMS: toRefreshTimeoutMS,
        });
        updateSetParameters({
            ShardingTaskExecutorPoolHostTimeoutMS: idleTimeoutMS1,
        });

        // Make ready connections
        configureReplSetFailpoint("waitInFindBeforeMakingBatch", "alwaysOn");
        launchFinds({times: conns, readPref: "primary"});
        configureReplSetFailpoint("waitInFindBeforeMakingBatch", "off");
        hasConnPoolStats({ready: conns});

        // Block refreshes and wait for the toRefresh timeout
        configureReplSetFailpoint("waitInIsMaster", "alwaysOn");
        sleep(toRefreshTimeoutMS);

        // Confirm that we're in pending for all of our conns
        hasConnPoolStats({pending: conns});

        // Set our min conns to 0 to make sure we don't refresh after pending timeout
        updateSetParameters({
            ShardingTaskExecutorPoolMinSize: 0,
        });

        // Wait for our pending timeout
        sleep(pendingTimeoutMS);
        hasConnPoolStats({});

        configureReplSetFailpoint("waitInIsMaster", "off");

        // Reset the min conns to make sure normal refresh doesn't extend the timeout
        updateSetParameters({
            ShardingTaskExecutorPoolMinSize: minConns,
        });

        // Wait for our host timeout and confirm the pool drops
        sleep(idleTimeoutMS1);
        hasConnPoolStats({isAbsent: true});

        // Reset the pool
        resetPools();

        // Sleep for a shorter timeout and then update so we're already expired
        sleep(idleTimeoutMS2);
        updateSetParameters({ShardingTaskExecutorPoolHostTimeoutMS: idleTimeoutMS2});
        hasConnPoolStats({isAbsent: true});
    });

    threads.forEach(function(thread) {
        thread.join();
    });

    st.stop();
})();
