/**
 * SERVER-58026: pin the audit of omittable FTDC sections.
 *
 * This test enforces the schema-stability contract proposed in
 * src/mongo/db/ftdc/SERVER-58026-omitted-sections-audit.md. It asserts:
 *
 *   (a) sections listed in the audit as "expected-present" appear in FTDC
 *       with at least one stable top-level field;
 *   (b) sections that the FTDCServerStatusCommandCollector explicitly
 *       suppresses ("expected-absent") remain absent;
 *   (c) the set of top-level keys under serverStatus.wiredTiger is
 *       identical across two consecutive FTDC samples — the load-bearing
 *       invariant for FTDC compression retention.
 *
 * Failing this test means a producer or collector change has reintroduced
 * a section whose schema is hostile to delta encoding, or has dropped a
 * section that should be steady-state present. Either update the audit
 * doc or fix the section.
 */
import {verifyGetDiagnosticData} from "jstests/libs/ftdc.js";

const conn = MongoRunner.runMongod();
const adminDb = conn.getDB("admin");

// Helpers ---------------------------------------------------------------

function topLevelKeys(obj) {
    return Object.keys(obj).sort();
}

function getFtdcSample() {
    return verifyGetDiagnosticData(adminDb);
}

// (a) Expected-present sections ----------------------------------------
//
// Standalone mongod registers serverStatus.wiredTiger and
// serverStatus.oplogTruncation unconditionally. Sections that depend on
// replica-set state (oplog, local.oplog.rs.stats) are not asserted here
// because this test runs against a standalone — they are pinned by
// replica-set tests in the same directory.

const sample = getFtdcSample();
assert(sample.hasOwnProperty("serverStatus"),
       "FTDC sample missing serverStatus: " + tojson(sample));
const ss = sample.serverStatus;

assert(ss.hasOwnProperty("wiredTiger"),
       "serverStatus.wiredTiger missing from FTDC sample: " + tojson(ss));
assert.gt(topLevelKeys(ss.wiredTiger).length, 0,
          "serverStatus.wiredTiger present but empty: " + tojson(ss.wiredTiger));

assert(ss.hasOwnProperty("oplogTruncation"),
       "serverStatus.oplogTruncation missing from FTDC sample: " + tojson(ss));
// Stable keys that must appear regardless of truncate-marker state.
for (const k of ["totalTimeTruncatingMicros", "truncateCount", "interruptCount"]) {
    assert(ss.oplogTruncation.hasOwnProperty(k),
           "serverStatus.oplogTruncation missing stable key '" + k + "': " +
               tojson(ss.oplogTruncation));
}

// (b) Expected-absent sections — collector-side suppression -------------
//
// FTDCServerStatusCommandCollector::collect explicitly disables these.

assert(!ss.hasOwnProperty("sharding"),
       "serverStatus.sharding should be suppressed in FTDC: " + tojson(ss));
assert(!ss.hasOwnProperty("timing"),
       "serverStatus.timing should be suppressed in FTDC: " + tojson(ss));
assert(!ss.hasOwnProperty("defaultRWConcern"),
       "serverStatus.defaultRWConcern should be suppressed in FTDC: " + tojson(ss));

if (ss.hasOwnProperty("metrics")) {
    assert(!ss.metrics.hasOwnProperty("apiVersions"),
           "serverStatus.metrics.apiVersions should be suppressed in FTDC: " +
               tojson(ss.metrics));
}

if (ss.hasOwnProperty("transactions")) {
    assert(!ss.transactions.hasOwnProperty("lastCommittedTransactions"),
           "serverStatus.transactions.lastCommittedTransactions should be " +
               "suppressed in FTDC: " + tojson(ss.transactions));
}

// (c) Schema stability across consecutive samples -----------------------
//
// FTDC delta encoding requires that the set of top-level keys under each
// section is stable across samples. If wiredTiger's permit acquisition
// flickers, this assertion catches the resulting schema churn.

const sample2 = getFtdcSample();
assert(sample2.hasOwnProperty("serverStatus") &&
           sample2.serverStatus.hasOwnProperty("wiredTiger"),
       "second FTDC sample missing serverStatus.wiredTiger: " + tojson(sample2));

const wtKeys1 = topLevelKeys(ss.wiredTiger);
const wtKeys2 = topLevelKeys(sample2.serverStatus.wiredTiger);

// Assert membership equality on the sorted key list. We compare as JSON
// strings rather than element-by-element because the assertion message
// then shows both lists in full when it fires.
assert.eq(tojson(wtKeys1), tojson(wtKeys2),
          "serverStatus.wiredTiger top-level key set changed between " +
              "consecutive FTDC samples — FTDC chunk schema is unstable. " +
              "sample1=" + tojson(wtKeys1) + " sample2=" + tojson(wtKeys2));

// Same stability assertion for oplogTruncation, which has multiple branches
// in its generator that can change the field set.
const otKeys1 = topLevelKeys(ss.oplogTruncation);
const otKeys2 = topLevelKeys(sample2.serverStatus.oplogTruncation);
assert.eq(tojson(otKeys1), tojson(otKeys2),
          "serverStatus.oplogTruncation top-level key set changed between " +
              "consecutive FTDC samples. sample1=" + tojson(otKeys1) +
              " sample2=" + tojson(otKeys2));

MongoRunner.stopMongod(conn);
