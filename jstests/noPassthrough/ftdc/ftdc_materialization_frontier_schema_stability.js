/**
 * SERVER-126520: The MaterializationFrontierServerStatus section iterates a
 * StringMap (absl::flat_hash_map) whose iteration order is non-deterministic.
 * Without a sort, the per-cell field order in serverStatus changes every
 * sample, causing FTDC to treat each sample as a new schema and emit a fresh
 * reference frame (~300x diagnostic file inflation).
 *
 * This test samples serverStatus.materializationFrontier N times via
 * getNextSample(), then asserts the field-name ordering is identical across
 * every sample. The test cleanly no-ops on builds that do not populate the
 * section (no zcell connections), so it can run in the standard
 * noPassthrough/ftdc suite.
 */
import {getNextSample, verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

const kNumSamples = 5;

const conn = MongoRunner.runMongod({
    setParameter: {
        diagnosticDataCollectionEnabled: true,
        diagnosticDataCollectionPeriodMillis: 200,
    },
});
const adminDb = conn.getDB("admin");

// Wait until FTDC has produced at least one sample.
verifyGetDiagnosticData(adminDb);

function getFrontierKeys(sample) {
    if (!sample.hasOwnProperty("serverStatus")) {
        return null;
    }
    const ss = sample.serverStatus;
    if (!ss.hasOwnProperty("materializationFrontier")) {
        return null;
    }
    // Object.keys() preserves insertion order, which mirrors BSON field order.
    const keys = Object.keys(ss.materializationFrontier);
    return keys.length === 0 ? null : keys;
}

const firstSample = getNextSample(adminDb);
const baselineKeys = getFrontierKeys(firstSample);

if (baselineKeys === null) {
    jsTestLog(
        "materializationFrontier section is absent or empty on this build/topology; " +
            "schema-stability assertion is vacuous. Skipping.",
    );
} else {
    jsTestLog("Baseline materializationFrontier keys (" + baselineKeys.length + "): " +
              tojson(baselineKeys));

    for (let i = 1; i < kNumSamples; ++i) {
        const sample = getNextSample(adminDb);
        const keys = getFrontierKeys(sample);
        assert.neq(
            keys,
            null,
            "materializationFrontier disappeared between samples; baseline had " +
                baselineKeys.length + " keys at sample 0, missing at sample " + i,
        );
        assert.eq(
            keys.length,
            baselineKeys.length,
            "materializationFrontier key count changed at sample " + i +
                ": baseline=" + baselineKeys.length + ", current=" + keys.length,
        );
        // Exact ordered comparison: any permutation triggers FTDC schema churn.
        assert.eq(
            keys,
            baselineKeys,
            "materializationFrontier field order changed at sample " + i +
                " (FTDC schema-change trigger). baseline=" + tojson(baselineKeys) +
                ", current=" + tojson(keys),
        );
    }
}

MongoRunner.stopMongod(conn);
