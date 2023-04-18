/**
 * Utilities for turning on/off and waiting for fail points.
 */

var configureFailPoint;
var configureFailPointForRS;
var kDefaultWaitForFailPointTimeout;

(function() {
"use strict";

if (configureFailPoint) {
    return;  // Protect against this file being double-loaded.
}

kDefaultWaitForFailPointTimeout = 5 * 60 * 1000;

configureFailPoint = function(conn, failPointName, data = {}, failPointMode = "alwaysOn") {
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
                conn.adminCommand({
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
                conn.adminCommand({configureFailPoint: failPointName, mode: "off"});
            }, "Timed out disabling fail point " + failPointName);
        }
    };
};

configureFailPointForRS = function(conns, failPointName, data = {}, failPointMode = "alwaysOn") {
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
                    conn.adminCommand({configureFailPoint: failPointName, mode: "off"});
                }, "Timed out disabling fail point " + failPointName);
            });
        }
    };
};
})();
