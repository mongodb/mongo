/**
 * Test for the ingressConnectionEstablishment rate limiter behavior when the client side
 * disconnects while queued.
 *
 * The default baton, which is used for gRPC, doesn't get marked as disconnected when the
 * client disconnects without an additional read or write on the socket.
 * @tags: [
 *      grpc_incompatible,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    isLinux,
} from "jstests/libs/os_helpers.js";
import {
    getConnectionStats,
    runTestReplSet,
    runTestShardedCluster,
    runTestStandaloneParamsSetAtRuntime,
    runTestStandaloneParamsSetAtStartup
} from "jstests/noPassthrough/network/libs/conn_establishment_rate_limiter_helpers.js";

const maxQueueSize = 3;

const testKillOnClientDisconnect = (conn) => {
    let connDelayFailPoint = configureFailPoint(conn, 'hangInRateLimiter');

    let queuedConn;
    try {
        queuedConn = new Mongo(`mongodb://${conn.host}/?socketTimeoutMS=1000`);
    } catch (e) {
        jsTestLog(e);
        assert(e.message.includes("Socket operation timed out"));
    }
    assert.eq(null, queuedConn, "Connection should not have been established");
    connDelayFailPoint.off();

    // The isConnected check will succeed if using the default baton because there is still data on
    // the socket to read, and the markKillOnClientDisconnect logic with the default baton checks
    // that rather than polling the socket state. Because of this, we don't assert on the log or
    // metric on non-Linux platforms.
    if (isLinux()) {
        assert.soon(() => checkLog.checkContainsOnceJson(
                        conn, 20883));  // Interrupted operation as its client disconnected
        assert.soon(() =>
                        (1 ==
                         getConnectionStats(
                             conn)["establishmentRateLimit"]["interruptedDueToClientDisconnect"]));
    }
};

const testKillOnClientDisconnectOpts = {
    ingressConnectionEstablishmentRateLimiterEnabled: true,
    ingressConnectionEstablishmentRatePerSec: 1,
    ingressConnectionEstablishmentBurstCapacitySecs: 1,
    ingressConnectionEstablishmentMaxQueueDepth: maxQueueSize,
};
runTestStandaloneParamsSetAtStartup(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
runTestStandaloneParamsSetAtRuntime(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
runTestReplSet(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
runTestShardedCluster(testKillOnClientDisconnectOpts, testKillOnClientDisconnect);
