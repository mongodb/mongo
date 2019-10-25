/**
 * Utilities for turning on/off and waiting for fail points.
 */

var configureFailPoint;

(function() {
"use strict";

if (configureFailPoint) {
    return;  // Protect against this file being double-loaded.
}

configureFailPoint = function(conn, failPointName, data = {}, failPointMode = "alwaysOn") {
    return {
        conn: conn,
        failPointName: failPointName,
        timesEntered: assert
                          .commandWorked(conn.adminCommand(
                              {configureFailPoint: failPointName, mode: failPointMode, data: data}))
                          .count,
        wait:
            function(additionalTimes = 1, maxTimeMS = 5 * 60 * 1000) {
                // Can only be called once because this function does not keep track of the
                // number of times the fail point is entered between the time it returns
                // and the next time it gets called.
                assert.commandWorked(conn.adminCommand({
                    waitForFailPoint: failPointName,
                    timesEntered: this.timesEntered + additionalTimes,
                    maxTimeMS: maxTimeMS
                }));
            },
        off:
            function() {
                assert.commandWorked(
                    conn.adminCommand({configureFailPoint: failPointName, mode: "off"}));
            }
    };
};
})();
