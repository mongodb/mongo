/**
 * Tests that OpenTelemetry metrics registered with ServerStatusOptions are reported under the
 * expected serverStatus paths.
 *
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {createMetricsDirectory} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

describe("OTel metrics reported under serverStatus", function () {
    before(function () {
        this.mongod = MongoRunner.runMongod();
        assert.commandWorked(this.mongod.getDB(jsTestName()).runCommand({ping: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("reports connectionsProcessed under metrics.network", function () {
        const db = this.mongod.getDB(jsTestName());
        // Open an extra ingress connection so the counter is guaranteed to have ticked in this
        // test rather than relying on ordering between test cases.
        const extraConn = new Mongo(this.mongod.host);
        assert.commandWorked(extraConn.getDB(jsTestName()).runCommand({ping: 1}));

        assert.soon(
            () => db.serverStatus().metrics.network.connectionsProcessed >= 1,
            () =>
                `Expected metrics.network.connectionsProcessed >= 1, got ${tojson(db.serverStatus().metrics.network)}`,
        );
    });
});
