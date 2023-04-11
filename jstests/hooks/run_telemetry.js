/**
 * Runs the $telemetry stage and ensures that all the expected fields are present.
 */

'use strict';

(function() {
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.

if (typeof db === 'undefined') {
    throw new Error(
        "Expected mongo shell to be connected a server, but global 'db' object isn't defined");
}

// Disable implicit sessions so tests that kill random sessions won't interrupt the operations in
// this test that aren't resilient to interruptions.
TestData.disableImplicitSessions = true;

const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

function runTelemetry(host) {
    function verifyFields(doc) {
        const kOtherTopLevelFields = [
            "key",
            "metrics",
            "asOf",
        ];
        const kFacetedMetricFields = [
            "docsReturned",
            "queryExecMicros",
        ];
        const kSubMetricFields = [
            "sum",
            "min",
            "max",
            "sumOfSquares",
        ];
        const kSingletonMetricFields = [
            "execCount",
            "firstSeenTimestamp",
            "lastExecutionMicros",
        ];
        for (let key of kOtherTopLevelFields) {
            assert(doc.hasOwnProperty(key),
                   `The telemetry output is missing the "${key}" property: ${tojson(doc)}`);
        }
        const metricsDoc = doc["metrics"];
        for (let key of kSingletonMetricFields) {
            assert(metricsDoc.hasOwnProperty(key),
                   `The telemetry 'metrics' output is missing the "${key}" property: ${
                       tojson(metricsDoc)}`);
        }
        for (let facetedMetric of kFacetedMetricFields) {
            assert(metricsDoc.hasOwnProperty(facetedMetric),
                   `The telemetry 'metrics' output is missing the "${facetedMetric}" property: ${
                       tojson(metricsDoc)}`);
            const subMetricsDoc = metricsDoc[facetedMetric];
            for (let subMetric of kSubMetricFields) {
                assert(
                    subMetricsDoc.hasOwnProperty(subMetric),
                    `The telemetry 'metrics' output is missing the ${subMetric} property from the ${
                        facetedMetric} subdoc: ${tojson(subMetricsDoc)}`);
            }
        }
    }

    let conn = new Mongo(host);
    conn.setSecondaryOk();

    assert.neq(
        null, conn, "Failed to connect to host '" + host + "' for background metrics collection");

    let db = conn.getDB("admin");
    function verifyResults(telemetryCursor) {
        while (telemetryCursor.hasNext()) {
            let doc = telemetryCursor.next();
            try {
                verifyFields(doc);
            } catch (e) {
                print(
                    "caught exception while verifying that all expected fields are in the metrics " +
                    "output: " + tojson(doc));
                throw (e);
            }
        }
    }
    verifyResults(db.aggregate([{$telemetry: {redactIdentifiers: false}}]));
    verifyResults(db.aggregate([{$telemetry: {redactIdentifiers: true}}]));
}

// This file is run continuously and is very fast so we want to impose some kind of rate limiting
// which is why we sleep for 1 second here. This sleep is here rather than in run_telemetry.py
// because the background job that file uses is designed to be run continuously so it is easier and
// cleaner to just sleep here.
// TODO SERVER-75983 move this logic into the test harness.
sleep(1000);
if (topology.type === Topology.kStandalone) {
    try {
        runTelemetry(topology.mongod);
    } catch (e) {
        print("background aggregate metrics against the standalone failed");
        throw e;
    }
} else if (topology.type === Topology.kReplicaSet) {
    for (let replicaMember of topology.nodes) {
        try {
            runTelemetry(replicaMember);
        } catch (e) {
            print("background aggregate metrics was not successful against all replica set " +
                  "members");
            throw e;
        }
    }
} else {
    throw new Error("Unsupported topology configuration: " + tojson(topology));
}
})();
