/**
 * Tests that OpenTelemetry metrics registered with ServerStatusOptions are reported under the
 * expected serverStatus paths.
 *
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";

describe("OTel metrics reported under serverStatus", function () {
    before(function () {
        this.mongod = MongoRunner.runMongod();
        assert.commandWorked(this.mongod.getDB(jsTestName()).runCommand({ping: 1}));
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("reports replicatedFastCount.isRunning under metrics.replicatedFastCount", function () {
        // replicatedFastCount.isRunning is an OTel gauge registered with serverStatusOptions. Its
        // value is 0 on a plain mongod (the background thread is not running), but it must be
        // present and non-negative to confirm the serverStatusOptions adapter is wired up correctly.
        const metrics = this.mongod.getDB(jsTestName()).serverStatus().metrics;
        assert.gte(
            metrics.replicatedFastCount.isRunning,
            0,
            "Expected metrics.replicatedFastCount.isRunning to be present and non-negative",
            {replicatedFastCount: metrics.replicatedFastCount},
        );
    });
});
