/**
 * Tests metrics and logging for the priority port.
 *
 * @tags: [
 *   requires_fcv_83,
 *   # The priority port is based on ASIO, so gRPC testing is excluded
 *   grpc_incompatible,
 * ]
 */
import {describe, before, after, it} from "jstests/libs/mochalite.js";
import {checkLog} from "src/mongo/shell/check_log.js";

describe("Tests metrics and logging for connections via the priority port", function () {
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

    function assertPriorityPortConnCountServerStatusMetricMatches(conn, expectedCount) {
        let currentCount;
        assert.soon(
            () => {
                let connectionMetrics = assert.commandWorked(conn.adminCommand({serverStatus: 1})).connections;
                currentCount = connectionMetrics.priority;
                return currentCount == expectedCount;
            },
            () => {
                return (
                    "Incorrect number of priority port connections: expected " +
                    expectedCount +
                    ", but serverStatus() reports " +
                    currentCount
                );
            },
        );
    }

    before(() => {
        this.conn = MongoRunner.runMongod({priorityPort: allocatePort(), bind_ip: "127.0.0.1", useHostname: false});
        this.host = this.conn.hostNoPort;
        this.mainPort = this.conn.port;
        this.priorityPort = this.conn.priorityPort;
    });

    after(() => {
        MongoRunner.stopMongod(this.conn);
    });

    it("Check that normal connections don't log or increment the priority metrics", () => {
        let conn = new Mongo(this.host + ":" + this.mainPort);

        const msg = /"isPriority":"true"/;
        assertDoesNotContain(conn, logID, msg);
        assertPriorityPortConnCountServerStatusMetricMatches(conn, 0);

        conn.close();
    });

    it("Check that priority port connections log and increment stats", () => {
        let mainConn = new Mongo(this.host + ":" + this.mainPort);
        let conns = [];
        for (let i = 1; i < 5; i++) {
            let newConn = new Mongo(this.host + ":" + this.priorityPort);

            const msg = /"isPriority":"true"/;
            assert(
                checkLog.checkContainsWithCountJson(mainConn, logID, {"isPriority": true}, i),
                "Expecting to see " + i + " instances of log " + logID + " with " + msg,
            );
            assertPriorityPortConnCountServerStatusMetricMatches(mainConn, i);

            conns.push(newConn);
        }
        conns.forEach((conn) => {
            conn.close();
        });
        assertPriorityPortConnCountServerStatusMetricMatches(mainConn, 0);
    });
});
