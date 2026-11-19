/**
 * Tests that WiredTiger statistics that are also exported to OTel still appear in serverStatus, and
 * that the values presented are sensible.
 *
 * @tags: [
 *   requires_otel_build,
 *   requires_wiredtiger,
 * ]
 */

import {
    awaitMetrics,
    otelFileExportParams,
} from "jstests/noPassthrough/observability/libs/otel_metrics_file_export_helpers.js";

const {metricsDir, otelParams} = otelFileExportParams(jsTestName());
const conn = MongoRunner.runMongod({
    setParameter: {
        ...otelParams,
        openTelemetryExportIntervalMillis: 500,
        openTelemetryExportTimeoutMillis: 200,
    },
});
const db = conn.getDB("test");
const coll = db.getCollection("t");
const pagesBeforeInsert = db.serverStatus().wiredTiger.cache["pages currently held in the cache"];

const numDocs = 10000;
const docs = [];
for (let i = 0; i < numDocs; ++i) {
    docs.push({_id: i, i: 0, x: "x".repeat(1000)});
}
assert.commandWorked(coll.insert(docs));
const insertsDone = new Date();

const otelName = "mongodb.serverStatus.wiredTiger.cache.pages_currently_held_in_the_cache";

const pagesAfterInsert = db.serverStatus().wiredTiger.cache["pages currently held in the cache"];
jsTest.log.info("serverStatus cache pages before and after insert", {
    before: pagesBeforeInsert,
    after: pagesAfterInsert,
});

assert.gt(
    pagesAfterInsert,
    pagesBeforeInsert,
    "serverStatus cache pages did not increase after insert",
);

function withinTenPercent(a, b) {
    return Math.abs(a - b) <= 0.1 * Math.max(a, b);
}
awaitMetrics(
    metricsDir,
    insertsDone,
    (m) => withinTenPercent(m[otelName]?.value ?? 0, pagesAfterInsert),
    (m) => `${otelName}=${m[otelName]?.value} vs serverStatus=${pagesAfterInsert}`,
);

MongoRunner.stopMongod(conn);
