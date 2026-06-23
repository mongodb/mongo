/**
 * Tests that the OTel counter `serverStatus.asserts` increments — broken down by the `kind`
 * attribute — when assertions fire, and does not increment for successful commands.
 *
 * @tags: [requires_otel_build]
 */

import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {
    getCounterByAttribute,
    getLatestMetrics,
    getLatestRawRecord,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_file_export_helpers.js";

const kMetricName = "serverStatus.asserts";
const kKinds = ["regular", "msg", "user", "tripwire"];

function readAllKinds(metricsDir) {
    const result = {};
    for (const kind of kKinds) {
        result[kind] = getCounterByAttribute(metricsDir, kMetricName, "kind", kind);
    }
    return result;
}

describe("OTel asserts metric file export", function () {
    before(function () {
        const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
        this.metricsDir = metricsDir;

        this.mongod = MongoRunner.runMongod({
            setParameter: {
                ...otelParams,
                openTelemetryExportIntervalMillis: 500,
                openTelemetryExportTimeoutMillis: 200,
            },
        });

        this.db = this.mongod.getDB("test");

        // Warm up: wait until at least one OTel export has been written before any counter reads.
        assert.soon(
            () => getLatestRawRecord(this.metricsDir) !== null,
            "No initial metrics export",
            30000,
            500,
        );
    });

    after(function () {
        MongoRunner.stopMongod(this.mongod);
    });

    it("increments asserts kind=user when commands uassert and leaves other kinds flat", function () {
        const baseline = readAllKinds(this.metricsDir);

        // An unknown top-level $-operator triggers a uassert with BadValue from the matcher.
        for (let i = 0; i < 2; ++i) {
            assert.commandFailedWithCode(
                this.db.runCommand({find: "coll", filter: {$invalidOp: 1}}),
                ErrorCodes.BadValue,
            );
        }

        // user counter should grow by >=2.
        assert.soon(
            () =>
                getCounterByAttribute(this.metricsDir, kMetricName, "kind", "user") >=
                baseline.user + 2,
            `Expected ${kMetricName} kind=user to increase by >=2 from ${baseline.user}`,
            30000,
            300,
        );

        // Other kinds should not move under the failing-find workload. The assert.soon above
        // already confirmed an export exists that shows the user increment; read other kinds from
        // the same latest snapshot rather than waiting for an additional export cycle (which would
        // extend the window for background-assertion noise to increment other kinds spuriously).
        const final = readAllKinds(this.metricsDir);
        for (const kind of ["regular", "msg", "tripwire"]) {
            assert.eq(
                final[kind],
                baseline[kind],
                `kind=${kind} should not increment for failing finds`,
                {
                    kind,
                    baseline: baseline[kind],
                    final: final[kind],
                },
            );
        }
    });

    it("does NOT increment any asserts kind on successful commands", function () {
        const baseline = readAllKinds(this.metricsDir);
        const baselineDate = new Date();

        assert.commandWorked(this.db.runCommand({ping: 1}));
        assert.commandWorked(this.db.runCommand({ping: 1}));

        // Wait for a fresh export window past the baseline so any delta from the pings would be
        // visible.
        assert.soon(
            () => (getLatestMetrics(this.metricsDir)?.time ?? 0) > baselineDate.getTime(),
            "No new OTel export after baselineDate",
            30000,
            200,
        );

        const final = readAllKinds(this.metricsDir);
        for (const kind of kKinds) {
            assert.eq(final[kind], baseline[kind], `kind=${kind} should not increment on ping`, {
                kind,
                baseline: baseline[kind],
                final: final[kind],
            });
        }
    });
});
