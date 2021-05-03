/**
 * Runs the $operationMetrics stage and ensures that all the expected fields are present.
 */

'use strict';

(function() {
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.

if (typeof db === 'undefined') {
    throw new Error(
        "Expected mongo shell to be connected a server, but global 'db' object isn't defined");
}

// Disable implicit sessions so FSM workloads that kill random sessions won't interrupt the
// operations in this test that aren't resilient to interruptions.
TestData.disableImplicitSessions = true;

const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

const aggregateMetricsBackground = function(host) {
    function verifyFields(doc) {
        const kTopLevelFields = [
            "docBytesWritten",
            "docUnitsWritten",
            "idxEntryBytesWritten",
            "idxEntryUnitsWritten",
            "totalUnitsWritten",
            "cpuNanos",
            "db",
            "primaryMetrics",
            "secondaryMetrics"
        ];
        const kReadFields = [
            "docBytesRead",
            "docUnitsRead",
            "idxEntryBytesRead",
            "idxEntryUnitsRead",
            "keysSorted",
            "docUnitsReturned"
        ];

        for (let key of kTopLevelFields) {
            assert(doc.hasOwnProperty(key), "The metrics output is missing the property: " + key);
        }
        let primaryMetrics = doc.primaryMetrics;
        for (let key of kReadFields) {
            assert(primaryMetrics.hasOwnProperty(key),
                   "The metrics output is missing the property: primaryMetrics." + key);
        }
        let secondaryMetrics = doc.secondaryMetrics;
        for (let key of kReadFields) {
            assert(secondaryMetrics.hasOwnProperty(key),
                   "The metrics output is missing the property: secondaryMetrics." + key);
        }
    }

    let conn = new Mongo(host);
    conn.setSecondaryOk();

    assert.neq(
        null, conn, "Failed to connect to host '" + host + "' for background metrics collection");

    // Filter out arbiters.
    if (conn.adminCommand({isMaster: 1}).arbiterOnly) {
        print("Skipping background aggregation against test node: " + host +
              " because it is an arbiter and has no data.");
        return;
    }

    let db = conn.getDB("admin");
    let clearMetrics = Math.random() < 0.9 ? false : true;
    print("Running $operationMetrics with {clearMetrics: " + clearMetrics + "} on host: " + host);
    const cursor = db.aggregate([{$operationMetrics: {clearMetrics: clearMetrics}}]);
    while (cursor.hasNext()) {
        let doc = cursor.next();
        try {
            verifyFields(doc);
        } catch (e) {
            print("caught exception while verifying that all expected fields are in the metrics " +
                  "output: " + tojson(doc));
            throw (e);
        }
    }
};

// This file is run continuously and is very fast so we want to impose some kind of rate limiting
// which is why we sleep for 1 second here. This sleep is here rather than in
// aggregate_metrics_background.py because the background job that file uses is designed to be run
// continuously so it is easier and cleaner to just sleep here.
sleep(1000);
if (topology.type === Topology.kStandalone) {
    try {
        aggregateMetricsBackground(topology.mongod);
    } catch (e) {
        print("background aggregate metrics against the standalone failed");
        throw e;
    }
} else if (topology.type === Topology.kReplicaSet) {
    for (let replicaMember of topology.nodes) {
        try {
            aggregateMetricsBackground(replicaMember);
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
