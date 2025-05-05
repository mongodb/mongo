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

/**
 * @typedef {Object} Args
 * @property {number} ready - The expected number of ready connections.
 * @property {number} pending - The expected number of pending connections.
 * @property {number} active - The expected number of active connections.
 * @property {Array} hosts - The list of hosts to check stats for.
 * @property {boolean} isAbsent - Whether the stats should be absent.
 * @property {function} checkStatsFunc - A custom assertion function.
 */

/**
 * Runs a connPoolStats and asserts values for each specified host. Can optionally run a command
 * besides connPoolStats.
 *
 * @param {Mongo} mongos - The connection to the mongos that will run the connPoolStats cmd.
 * @param {Array} allHosts - The list of hosts to assert stats on.
 * @param {Object} args - The arguments used for assertion.
 * @param {number} checkNum - A counter used to identify the current invocation.
 * @param {string} connPoolStatsCmd - Optionally supplies a command to run instead of connPoolStats.
 * @returns {number} - The updated check number.
 */
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

/**
 * Checks whether the host has any open connections.
 *
 * @param {*} connPoolHostStats - Connection pool stats for an individual host.
 * @returns {boolean} - True if the host has open connections, false otherwise.
 */
export function checkHostHasOpenConnections(connPoolHostStats) {
    return connPoolHostStats.inUse + connPoolHostStats.available + connPoolHostStats.leased +
        connPoolHostStats.refreshing >
        0;
}

/**
 * Checks whether the host has no open connections.
 *
 * @param {*} connPoolHostStats - Connection pool stats for an individual host.
 * @returns {boolean} - True if the host has no open connections, false otherwise.
 */
export function checkHostHasNoOpenConnections(connPoolHostStats) {
    return checkHostHasOpenConnections(connPoolHostStats) === false;
}
