/**
 * Tests metrics and logging for the maintenance port.
 *
 * @tags: [
 *   featureFlagDedicatedPortForMaintenanceOperations,
 * ]
 */
import {describe, before, after, it} from "jstests/libs/mochalite.js";
import {checkLog} from "src/mongo/shell/check_log.js";

describe("Tests metrics and logging for connections via the maintenance port", function () {
    const logID = 22943;

    function assertDoesNotContain(conn, id, msg) {
        const logs = checkLog.getGlobalLog(conn);
        let containsLog = logs.some((log) => {
            if (log.search(`"id":${id},`) != -1) {
                return log.search(msg) != -1;
            } else {
                return false;
            }
        });
        assert.neq(containsLog, true, "Found log line when none should exist" + tojson(msg));
    }

    function assertMaintenancePortConnCountServerStatusMetricMatches(conn, expectedCount) {
        let currentCount;
        assert.soon(
            () => {
                let connectionMetrics = assert.commandWorked(conn.adminCommand({serverStatus: 1})).connections;
                currentCount = connectionMetrics.maintenance;
                return currentCount == expectedCount;
            },
            () => {
                return (
                    "Incorrect number of maintenance port connections: expected " +
                    expectedCount +
                    ", but serverStatus() reports " +
                    currentCount
                );
            },
        );
    }

    before(() => {
        this.conn = MongoRunner.runMongod({maintenancePort: allocatePort(), bind_ip: "127.0.0.1", useHostname: false});
        this.host = this.conn.hostNoPort;
        this.mainPort = this.conn.port;
        this.maintenancePort = this.conn.maintenancePort;
    });

    after(() => {
        MongoRunner.stopMongod(this.conn);
    });

    it("Check that normal connections don't log or increment the maintenance metrics", () => {
        let conn = new Mongo(this.host + ":" + this.mainPort);

        const msg = /"isMaintenance":"true"/;
        assertDoesNotContain(conn, logID, msg);
        assertMaintenancePortConnCountServerStatusMetricMatches(conn, 0);

        conn.close();
    });

    it("Check that maintenance port connections log and increment stats", () => {
        let mainConn = new Mongo(this.host + ":" + this.mainPort);
        let conns = [];
        for (let i = 1; i < 5; i++) {
            let newConn = new Mongo(this.host + ":" + this.maintenancePort);

            const msg = /"isMaintenance":"true"/;
            assert(
                checkLog.checkContainsWithCountJson(mainConn, logID, {"isMaintenance": true}, i),
                "Expecting to see " + i + " instances of log " + logID + " with " + msg,
            );
            assertMaintenancePortConnCountServerStatusMetricMatches(mainConn, i);

            conns.push(newConn);
        }
        conns.forEach((conn) => {
            conn.close();
        });
        assertMaintenancePortConnCountServerStatusMetricMatches(mainConn, 0);
    });
});
