/**
 * @tags: [
 *  requires_replication,
 * ]
 */

import {assertHasConnPoolStats, launchFinds} from "jstests/libs/conn_pool_helpers.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kDbName = "test";
const kCollName = "testColl";
const kDefaultPoolMinSize = 1;
const kDefaultPoolMaxSize = 4;

let currentCheckNum = 0;

function setMaxPoolSize(conn, size) {
    return conn.adminCommand({"setParameter": 1, mirrorReadsMaxConnPoolSize: size});
}

function getMaxPoolSize(conn) {
    return conn.adminCommand({"getParameter": 1, mirrorReadsMaxConnPoolSize: 1});
}

function updateMaxPoolSizeAndVerify(conn, size) {
    assert.commandWorked(setMaxPoolSize(conn, size));
    const res = assert.commandWorked(getMaxPoolSize(conn));
    assert.eq(res.mirrorReadsMaxConnPoolSize, size);
}

function runTest(conn, secondaryHosts, numFindQueries, expectedConns, checkStatsFunc = undefined) {
    let threads = [];
    launchFinds(conn, threads, {times: numFindQueries, readPref: "primary"});
    threads.forEach(function(thread) {
        thread.join();
    });

    let args = {ready: expectedConns};
    if (checkStatsFunc !== undefined) {
        args.checkStatsFunc = checkStatsFunc;
    }

    currentCheckNum = assertHasConnPoolStats(
        conn, secondaryHosts, args, currentCheckNum, "_mirrorMaestroConnPoolStats");
}

function testMinAndMax(primary, secondaries) {
    primary.adminCommand(
        {configureFailPoint: "connectionPoolAlwaysRequestsNewConn", mode: "alwaysOn"});

    const coll = primary.getDB(kDbName)[kCollName];
    assert.commandWorked(coll.insert({x: 1}));

    let hostsToAssertStatsOn = [];
    for (let secondary of secondaries) {
        hostsToAssertStatsOn.push(secondary.name);
    }

    // Launch an initial find query to trigger to min. Since some internal replset operations may
    // also be mirrored during this test, we have to assert on a range of available connections
    // rather than a specific number.
    runTest(
        primary, hostsToAssertStatsOn, kDefaultPoolMinSize, kDefaultPoolMinSize, function(stats) {
            return stats.available >= kDefaultPoolMinSize && stats.available <= kDefaultPoolMaxSize;
        });

    // Launch find quieries to fill the pool to max.
    const numFindQueries = kDefaultPoolMaxSize + 20;
    runTest(primary, hostsToAssertStatsOn, numFindQueries, kDefaultPoolMaxSize);

    // Increase pool size by 1.
    updateMaxPoolSizeAndVerify(primary, kDefaultPoolMaxSize + 1);
    runTest(primary, hostsToAssertStatsOn, numFindQueries, kDefaultPoolMaxSize + 1);

    // Decrease max pool size to min.
    updateMaxPoolSizeAndVerify(primary, kDefaultPoolMinSize);
    assert.commandWorked(primary.adminCommand(
        {_dropMirrorMaestroConnections: 1, hostAndPort: hostsToAssertStatsOn}));
    runTest(primary, hostsToAssertStatsOn, numFindQueries, kDefaultPoolMinSize);

    // Invalid max pool size.
    assert.commandFailedWithCode(setMaxPoolSize(primary, 0), ErrorCodes.BadValue);
}

const rst = new ReplSetTest({nodes: 3});
rst.startSet({setParameter: {mirrorReads: {samplingRate: 1}}});
rst.initiate();

testMinAndMax(rst.getPrimary(), rst.getSecondaries());
rst.stopSet();
