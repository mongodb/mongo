/**
 * Utilities for turning on/off and waiting for fail points.
 */

import {isMongos} from "jstests/concurrency/fsm_workload_helpers/server_types.js";

export var configureFailPoint;

export var configureFailPointForRS;
export var kDefaultWaitForFailPointTimeout;

/**
 * Utility to get the correct name of a fail point given the name of the fail point on the main
 * branch and the wire-version of the server you are speaking to.
 */
export var getFailPointName;

export var getActualFailPointName;

(function() {
if (configureFailPoint) {
    return;  // Protect against this file being double-loaded.
}

const kWireVersion73 = 24;

// This table is the opposite to failPointRenameTable below. It maps from a fail point name to
// either a router-specific or a shard-specific version, based on the type of connection that is
// passed to configureFailPoint.
const failPointRouterShardRenameTable = {
    "waitInFindBeforeMakingBatch":
        {router: "routerWaitInFindBeforeMakingBatch", shard: "shardWaitInFindBeforeMakingBatch"},
};

getActualFailPointName = function(conn, failPointName, getShard) {
    const names = failPointRouterShardRenameTable[failPointName];
    if (!names) {
        return failPointName;
    }
    conn = typeof conn.getMongo == "function" ? conn.getMongo() : conn;
    const wireVersion = conn.getMaxWireVersion();
    if (wireVersion <= kWireVersion73) {  // Only correct in 8.0 .
        return failPointName;
    }
    if (getShard !== undefined) {
        return getShard ? names.shard : names.router;
    }
    return isMongos(conn.getDB("admin")) ? names.router : names.shard;
};

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
        wait: function({maxTimeMS = kDefaultWaitForFailPointTimeout, timesEntered = 1} = {}) {
            // Can only be called once because this function does not keep track of the
            // number of times the fail point is entered between the time it returns
            // and the next time it gets called.
            sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
                return conn.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: this.timesEntered + timesEntered,
                    maxTimeMS: maxTimeMS
                });
            }, "Timed out waiting for failpoint " + failPointName);
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
    var failPointNames = [];
    conns.forEach((conn, index) => {
        var actualName = getActualFailPointName(conn, failPointName, true /* getShard */);
        failPointNames[index] = actualName;
        sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
            return conn.adminCommand(
                {configureFailPoint: actualName, mode: failPointMode, data: data});
        }, "Timed out setting failpoint " + actualName);
    });

    return {
        conns: conns,
        failPointNames: failPointNames,
        off: function() {
            conns.forEach((conn, index) => {
                var actualName = failPointNames[index];
                sh.assertRetryableCommandWorkedOrFailedWithCodes(() => {
                    return conn.adminCommand({configureFailPoint: actualName, mode: "off"});
                }, "Timed out disabling fail point " + actualName);
            });
        }
    };
};

// Add an entry to this map if you are changing the name of an existing fail point.
// Key is name of the failpoint on master. Value is a function taking wireVersion
// that returns the correct name of the fail point on that wireVersion.
const failPointRenameTable = {
    "routerWaitInHello": function(wireVersion) {
        if (wireVersion >= kWireVersion73) {
            return "routerWaitInHello";
        }
        return "waitInHello";
    },
    "shardWaitInHello": function(wireVersion) {
        if (wireVersion >= kWireVersion73) {
            return "shardWaitInHello";
        }
        return "waitInHello";
    }
};

getFailPointName = function(failPointNameOnMain, wireVersion) {
    return failPointRenameTable[failPointNameOnMain](wireVersion);
};
})();
