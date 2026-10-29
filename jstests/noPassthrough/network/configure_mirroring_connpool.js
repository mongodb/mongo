/**
 * @tags: [
 *  requires_replication,
 * ]
 */

import {assertHasConnPoolStats, launchFinds} from "jstests/libs/network/conn_pool_helpers.js";
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

function connCount(stats) {
    return stats.available + stats.inUse;
}

function checkConnsInRange(minConns, maxConns) {
    return function (stats) {
        const n = connCount(stats);
        return n >= minConns && n <= maxConns;
    };
}

function runTest(conn, secondaryHosts, numFindQueries, minConns, maxConns) {
    let threads = [];
    launchFinds(conn, threads, {times: numFindQueries, readPref: "primary"});
    threads.forEach(function (thread) {
        thread.join();
    });

    // Count available and in-use together so the check is stable while mirrored ops check
    // connections out and return them. Depending on the scheduler, traffic profile, etc, the pool
    // may vary the number of connections, so assert a range rather than an exact size.
    currentCheckNum = assertHasConnPoolStats(
        conn,
        secondaryHosts,
        {checkStatsFunc: checkConnsInRange(minConns, maxConns)},
        currentCheckNum,
        "_mirrorMaestroConnPoolStats",
    );
}

function testMinAndMax(primary, secondaries) {
    primary.adminCommand({
        configureFailPoint: "connectionPoolAlwaysRequestsNewConn",
        mode: "alwaysOn",
    });

    const coll = primary.getDB(kDbName)[kCollName];
    assert.commandWorked(coll.insert({x: 1}));

    let hostsToAssertStatsOn = [];
    for (let secondary of secondaries) {
        hostsToAssertStatsOn.push(secondary.name);
    }

    // Launch an initial find query to trigger the min.
    runTest(
        primary,
        hostsToAssertStatsOn,
        kDefaultPoolMinSize,
        kDefaultPoolMinSize,
        kDefaultPoolMaxSize,
    );

    // Launch find queries to create connection demand for the pool.
    const numFindQueries = kDefaultPoolMaxSize + 20;
    runTest(
        primary,
        hostsToAssertStatsOn,
        numFindQueries,
        kDefaultPoolMinSize,
        kDefaultPoolMaxSize,
    );

    // Verify the pool size stays within a larger maximum value.
    const updatedMaxSize = kDefaultPoolMaxSize + 1;
    updateMaxPoolSizeAndVerify(primary, updatedMaxSize);
    runTest(primary, hostsToAssertStatsOn, numFindQueries, kDefaultPoolMinSize, updatedMaxSize);

    // Decrease max pool size to min.
    updateMaxPoolSizeAndVerify(primary, kDefaultPoolMinSize);
    assert.commandWorked(
        primary.adminCommand({_dropMirrorMaestroConnections: 1, hostAndPort: hostsToAssertStatsOn}),
    );
    runTest(
        primary,
        hostsToAssertStatsOn,
        numFindQueries,
        kDefaultPoolMinSize,
        kDefaultPoolMinSize,
    );

    // Invalid max pool size.
    assert.commandFailedWithCode(setMaxPoolSize(primary, 0), ErrorCodes.BadValue);
}

const rst = new ReplSetTest({nodes: 3});
rst.startSet({setParameter: {mirrorReads: {samplingRate: 1}}});
rst.initiate();

testMinAndMax(rst.getPrimary(), rst.getSecondaries());
rst.stopSet();
