// To be used with set_read_preference_secondary.js and implicit_enable_profiler.js in suites
// that read directly from secondaries in a replica set. Check the profiler collections of all
// databases at the end of the suite to verify that each secondary only ran the read commands it
// got directly from the shell.

assert(TestData.connectDirectlyToRandomSubsetOfSecondaries);

import {DiscoverTopology, Topology} from "jstests/libs/discover_topology.js";

assert.eq(typeof db, "object", "Invalid `db` object, is the shell connected to a mongod?");
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());

if (topology.type !== Topology.kReplicaSet) {
    throw new Error("Unrecognized topology format:" + tojsononeline(topology));
}

const hostColl = db.getSiblingDB("config").connectDirectlyToSecondaries.hosts;
const hostDocs = hostColl.find().toArray();
assert.gt(hostDocs.length, 0, "Could not find information about direct secondary reads");
print("Validating profiler collections on hosts " + tojsononeline(hostDocs));

const numProfilerDocsPerHost = {};

function validateProfilerCollections(hostDoc) {
    print("Validating profiler collections on host " + tojsononeline(hostDoc));
    const conn = new Mongo(hostDoc.host);
    conn.setSecondaryOk();
    jsTest.authenticate(conn);

    const dbNames = conn.getDBNames();
    for (let dbName of dbNames) {
        const profilerColl = conn.getDB(dbName).system.profile;

        const profilerDocs =
            profilerColl.find({ns: {$ne: dbName + ".system.profile"}, appName: "MongoDB Shell"})
                .toArray();
        numProfilerDocsPerHost[hostDoc.host] += profilerDocs.length;

        jsTest.log("Validating profiler collection for database '" + dbName + "' on host " +
                   tojsononeline({...hostDoc, numDocs: profilerDocs.length}));
        if (hostDoc.isExcluded) {
            // Verify that this host didn't run any commands from the shell since it was excluded.
            assert.eq(profilerDocs.length, 0, profilerDocs);
        }

        // Verify that this host didn't run any commands from the shell that were supposed to run on
        // other hosts.
        hostDocs.forEach(otherHostDoc => {
            if (otherHostDoc.host == hostDoc.host) {
                return;
            }
            if (!otherHostDoc.hasOwnProperty("comment")) {
                assert(otherHostDoc.isPrimary || otherHostDoc.isExcluded, otherHostDoc);
                return;
            }
            const profilerDocs =
                profilerColl
                    .find({ns: {$ne: dbName + ".system.profile"}, comment: otherHostDoc.comment})
                    .toArray();
            assert.eq(profilerDocs.length, 0, profilerDocs);
        });
    }
}

hostDocs.forEach(hostDoc => {
    numProfilerDocsPerHost[hostDoc.host] = 0
    validateProfilerCollections(hostDoc);
});
// Sanity check to verify that profiling is enabled.
let totalNumProfilerDocs = 0;
for (let host in numProfilerDocsPerHost) {
    totalNumProfilerDocs += numProfilerDocsPerHost[host];
}
jsTest.log("Finished validating profiler collections on hosts " +
           tojsononeline({numProfilerDocsPerHost}));
assert.gt(totalNumProfilerDocs, 0);
