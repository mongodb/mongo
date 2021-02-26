/**
 * Utilities for turning on/off and waiting for fail points.
 */

var configureFailPoint;
var kDefaultWaitForFailPointTimeout;

(function() {
"use strict";

if (configureFailPoint) {
    return;  // Protect against this file being double-loaded.
}

kDefaultWaitForFailPointTimeout = 5 * 60 * 1000;

configureFailPoint = function(conn, failPointName, data = {}, failPointMode = "alwaysOn") {
    return {
        conn: conn,
        failPointName: failPointName,
        timesEntered: assert
                          .commandWorked(conn.adminCommand(
                              {configureFailPoint: failPointName, mode: failPointMode, data: data}))
                          .count,
        wait:
            function(maxTimeMS = kDefaultWaitForFailPointTimeout) {
                // Can only be called once because this function does not keep track of the
                // number of times the fail point is entered between the time it returns
                // and the next time it gets called.
                assert.commandWorked(conn.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: this.timesEntered + 1,
                    maxTimeMS: maxTimeMS
                }));
            },
        waitWithTimeout:
            function(timeoutMS) {
                // This function has three possible outcomes:
                //
                // 1) Returns true when the failpoint was hit.
                // 2) Returns false when the command returned a `MaxTimeMSExpired` response.
                // 3) Otherwise, this throws for an unexpected error.
                let res = assert.commandWorkedOrFailedWithCode(conn.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: this.timesEntered + 1,
                    maxTimeMS: timeoutMS
                }),
                                                               ErrorCodes.MaxTimeMSExpired);
                return res["ok"] === 1;
            },
        off:
            function() {
                assert.commandWorked(
                    conn.adminCommand({configureFailPoint: failPointName, mode: "off"}));
            }
    };
};
})();
