// To be used with set_read_preference_secondary.js and implicit_enable_profiler.js in suites
// that read directly from secondaries in a replica set. Check the profiler collections of all
// databases at the end of the suite to verify that each secondary only ran the read commands it
// got directly from the shell.

import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";
import {
    getTotalNumProfilerDocs,
    validateProfilerCollections
} from "jstests/noPassthrough/rs_endpoint/lib/validate_direct_secondary_reads.js";

assert(TestData.connectDirectlyToRandomSubsetOfSecondaries);

assert.eq(typeof db, "object", "Invalid `db` object, is the shell connected to a mongod?");
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
if (topology.type !== Topology.kReplicaSet) {
    throw new Error("Unrecognized topology format:" + tojsononeline(topology));
}

const hostColl = db.getSiblingDB("config").connectDirectlyToSecondaries.hosts;
const hostDocs = hostColl.find().toArray();
assert.gt(hostDocs.length, 0, "Could not find information about direct secondary reads");
print("Validating profiler collections on hosts " + tojsononeline(hostDocs));

// Count the number of profiler docs to verify that that profiling is enabled.
const numProfilerDocsPerHost = {};
hostDocs.forEach(hostDoc => {
    validateProfilerCollections(hostDoc, hostDocs, numProfilerDocsPerHost);
});
jsTest.log("Finished validating profiler collections on hosts " +
           tojsononeline({numProfilerDocsPerHost}));
assert.gt(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);
