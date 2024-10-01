/**
 * Utilities for turning on/off and waiting for fail points.
 */

import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export var configureFailPoint;

export var configureFailPointForRS;
export var kDefaultWaitForFailPointTimeout;

(function() {
if (configureFailPoint) {
    return;  // Protect against this file being double-loaded.
}

// This table is the opposite to failPointRenameTable below. It maps from a fail point name to
// either a router-specific or a shard-specific version, based on the type of connection that is
// passed to configureFailPoint.
const failPointRouterShardRenameTable = {
    "waitInFindBeforeMakingBatch":
        {router: "routerWaitInFindBeforeMakingBatch", shard: "shardWaitInFindBeforeMakingBatch"},
};

function getActualFailPointName(conn, failPointName) {
    const names = failPointRouterShardRenameTable[failPointName];
    if (!names) {
        return failPointName;
    }
    const isRouter =
        typeof conn.getMongo == "function" ? isMongos(conn) : isMongos(conn.getDB("admin"));
    return isRouter ? names.router : names.shard;
}

function getShardFailPointName(failPointName) {
    const names = failPointRouterShardRenameTable[failPointName];
    if (!names) {
        return failPointName;
    }
    return names.shard;
}

kDefaultWaitForFailPointTimeout = 10 * 60 * 1000;

configureFailPoint = function(conn, failPointName, data = {}, failPointMode = "alwaysOn") {
    failPointName = getActualFailPointName(conn, failPointName);

    const res = sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
        return conn.adminCommand(
            {configureFailPoint: failPointName, mode: failPointMode, data: data});
    }, "Timed out enabling fail point " + failPointName);

    return {
        conn: conn,
        failPointName: failPointName,
        timesEntered: res.count,
        wait: function({
            maxTimeMS = kDefaultWaitForFailPointTimeout,
            timesEntered = 1,
            expectedErrorCodes = []
        } = {}) {
            // Can only be called once because this function does not keep track of the
            // number of times the fail point is entered between the time it returns
            // and the next time it gets called.
            // This function has three possible outcomes:
            //
            // 1) Returns true when the failpoint was hit.
            // 2) Returns false when the command returned one of the expected error codes.
            // 3) Otherwise, this throws for an unexpected error.
            let res = sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
                return conn.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: this.timesEntered + timesEntered,
                    maxTimeMS: maxTimeMS
                });
            }, "Timed out waiting for failpoint " + failPointName, expectedErrorCodes);
            return res !== undefined && res["ok"] === 1;
        },
        waitWithTimeout: function(timeoutMS) {
            // This function has three possible outcomes:
            //
            // 1) Returns true when the failpoint was hit.
            // 2) Returns false when the command returned a `MaxTimeMSExpired` response.
            // 3) Otherwise, this throws for an unexpected error.
            let res = sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
                return conn.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: this.timesEntered + 1,
                    maxTimeMS: timeoutMS
                });
            }, "Timed out waiting for failpoint " + failPointName, [ErrorCodes.MaxTimeMSExpired]);
            return res !== undefined && res["ok"] === 1;
        },
        off: function() {
            sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
                return conn.adminCommand({configureFailPoint: failPointName, mode: "off"});
            }, "Timed out disabling fail point " + failPointName);
        }
    };
};

configureFailPointForRS = function(conns, failPointName, data = {}, failPointMode = "alwaysOn") {
    failPointName = getShardFailPointName(failPointName);

    conns.forEach((conn) => {
        sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
            return conn.adminCommand(
                {configureFailPoint: failPointName, mode: failPointMode, data: data});
        }, "Timed out setting failpoint " + failPointName);
    });

    return {
        conns: conns,
        failPointName: failPointName,
        off: function() {
            conns.forEach((conn) => {
                sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
                    return conn.adminCommand({configureFailPoint: failPointName, mode: "off"});
                }, "Timed out disabling fail point " + failPointName);
            });
        }
    };
};
})();
