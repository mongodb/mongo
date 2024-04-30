import {Thread} from "jstests/libs/parallelTester.js";

export function launchFinds(mongos, threads, {times, readPref, shouldFail}) {
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

export function assertHasConnPoolStats(
    mongos, allHosts, args, checkNum, connPoolStatsCmd = undefined) {
    checkNum++;
    jsTestLog("Check #" + checkNum + ": " + tojson(args));
    let {ready = 0, pending = 0, active = 0, hosts = allHosts, isAbsent, checkStatsFunc} = args;
    checkStatsFunc = checkStatsFunc ? checkStatsFunc : function(stats) {
        return stats.available == ready && stats.refreshing == pending &&
            (stats.inUse + stats.leased) == active;
    };

    function checkStats(res, host) {
        let stats = res.hosts[host];
        if (!stats) {
            jsTestLog("Connection stats for " + host + " are absent");
            return isAbsent;
        }

        jsTestLog("Connection stats for " + host + ": " + tojson(stats));
        return checkStatsFunc(stats);
    }

    function checkAllStats() {
        let cmdName = connPoolStatsCmd ? connPoolStatsCmd : "connPoolStats";
        let res = mongos.adminCommand({[cmdName]: 1});
        return hosts.map(host => checkStats(res, host)).every(x => x);
    }

    assert.soon(checkAllStats, "Check #" + checkNum + " failed", 10000);
    jsTestLog("Check #" + checkNum + " successful");
    return checkNum;
}
