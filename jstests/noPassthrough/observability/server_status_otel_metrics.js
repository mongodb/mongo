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

    it("reports replicatedFastCount.tailer.isRunning under metrics.replicatedFastCount", function () {
        // replicatedFastCount.tailer.isRunning is an OTel gauge registered with
        // serverStatusOptions. Its value is 0 on a plain mongod (the background thread is not
        // running), but it must be present and non-negative to confirm the serverStatusOptions
        // adapter is wired up correctly.
        const metrics = this.mongod.getDB(jsTestName()).serverStatus().metrics;
        assert.gte(
            metrics.replicatedFastCount.tailer.isRunning,
            0,
            "Expected metrics.replicatedFastCount.tailer.isRunning to be present and non-negative",
            {replicatedFastCount: metrics.replicatedFastCount},
        );
    });

    it("reports the internode hash mismatch counters under metrics.repl.internodeConsistency", function () {
        // These counters only advance when a non-primary disagrees with a document hash the primary
        // recorded, so on a plain mongod they must be present and zero.
        const metrics = this.mongod.getDB(jsTestName()).serverStatus().metrics;
        const hashMismatch = metrics.repl.internodeConsistency.hashMismatch;
        for (const opType of ["insert", "update", "delete"]) {
            assert.eq(
                hashMismatch[opType],
                0,
                `Expected ${opType} counter to be present and zero`,
                {
                    hashMismatch,
                },
            );
        }
    });
});
