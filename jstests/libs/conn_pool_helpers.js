load("jstests/libs/parallelTester.js");

function configureReplSetFailpoint(st, kDbName, failpoint, modeValue) {
    st.rs0.nodes.forEach(function(node) {
        assert.commandWorked(node.getDB("admin").runCommand({
            configureFailPoint: failpoint,
            mode: modeValue,
            data: {
                shouldCheckForInterrupt: true,
                nss: kDbName + ".test",
            },
        }));
    });
}

function launchFinds(mongos, threads, {times, readPref, shouldFail}) {
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
        }, mongos.host, readPref, 'test', shouldFail);
        thread.start();
        threads.push(thread);
    }
}

function assertHasConnPoolStats(mongos, allHosts, args, checkNum) {
    checkNum++;
    jsTestLog("Check #" + checkNum + ": " + tojson(args));
    var {ready = 0, pending = 0, active = 0, hosts = allHosts, isAbsent, checkStatsFunc} = args;
    checkStatsFunc = checkStatsFunc ? checkStatsFunc : function(stats) {
        return stats.available == ready && stats.refreshing == pending && stats.inUse == active;
    };

    function checkStats(res, host) {
        var stats = res.hosts[host];
        if (!stats) {
            jsTestLog("Connection stats for " + host + " are absent");
            return isAbsent;
        }

        jsTestLog("Connection stats for " + host + ": " + tojson(stats));
        return checkStatsFunc(stats);
    }

    function checkAllStats() {
        var res = mongos.adminCommand({connPoolStats: 1});
        return hosts.map(host => checkStats(res, host)).every(x => x);
    }

    assert.soon(checkAllStats, "Check #" + checkNum + " failed", 10000);
    jsTestLog("Check #" + checkNum + " successful");
    return checkNum;
}
