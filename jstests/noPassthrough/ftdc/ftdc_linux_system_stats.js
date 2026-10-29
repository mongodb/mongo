/**
 * Wait for FTDC periodic systemMetrics collection on Linux.
 * Values of the collected stats are host-specific and are not asserted.
 *
 * @tags: [
 *   # TODO (SERVER-100639): Remove this tag. The config fuzzer may disable FTDC, which would cause
 *   # this test to fail.
 *   does_not_support_config_fuzzer,
 * ]
 */
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {getNextSample, verifyGetDiagnosticData} from "jstests/libs/ftdc.js";
import {isLinux} from "jstests/libs/server_security/os_helpers.js";

if (!isLinux()) {
    jsTest.log.info("Skipping test because FTDC ethtool collection is Linux-only");
    quit();
}

describe("FTDC Linux system stats collection", function () {
    before(function () {
        this.conn = MongoRunner.runMongod({
            setParameter: {
                diagnosticDataCollectionEnabled: true,
                diagnosticDataCollectionPeriodMillis: 100,
            },
        });
        this.adminDb = this.conn.getDB("admin");
    });

    after(function () {
        MongoRunner.stopMongod(this.conn);
    });

    it("collects systemMetrics", function () {
        let sample = verifyGetDiagnosticData(this.adminDb);
        if (!sample.hasOwnProperty("systemMetrics")) {
            assert.soon(
                () => {
                    sample = getNextSample(this.adminDb);
                    return sample.hasOwnProperty("systemMetrics");
                },
                "Timeout waiting for FTDC systemMetrics",
                30 * 1000,
            );
        }

        assert(sample.systemMetrics.hasOwnProperty("cpu"), "systemMetrics.cpu missing", {sample});
        assert(sample.systemMetrics.hasOwnProperty("memory"), "systemMetrics.memory missing", {
            sample,
        });
        assert(sample.systemMetrics.hasOwnProperty("ethtool"), "systemMetrics.ethtool missing", {
            sample,
        });

        // A second cycle exercises get_stats after string names have been cached.
        getNextSample(this.adminDb);
    });
});
