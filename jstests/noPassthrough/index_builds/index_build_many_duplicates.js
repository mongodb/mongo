/**
 * This test inserts documents with large, duplicated keys. This will result in significant
 * fragmentation in the buffer allocator for index builds because we generate keys that end up being
 * deduplicated, but can still pin memory. We want to ensure that we track this fragmented memory
 * and spill often to avoid exceeding any memory limits.
 *
 * @tags: [
 *  # TODO(SERVER-107050): Enable this test after making primary-driven index builds respect
 *  # maxIndexBuildMemoryUsageMegabytes. Primary-driven index builds don't respect
 *  # maxIndexBuildMemoryUsageMegabytes, so this test will lead to OOMs during primary-driven index
 *  # builds.
 *  primary_driven_index_builds_incompatible,
 *  requires_replication,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const maxMemUsageMB = 50;
const replSet = new ReplSetTest({
    nodes: 1,
    nodeOptions: {setParameter: {maxIndexBuildMemoryUsageMegabytes: maxMemUsageMB}},
});
replSet.startSet();
replSet.initiate();

const primary = replSet.getPrimary();
const testDB = primary.getDB("test");
const coll = testDB[jsTestName()];

// Create documents with many large, duplicated keys.
const docs = 10 * 1000;
const bigArr = new Array(100).fill("x");
const bulk = coll.initializeUnorderedBulkOp();
for (let i = 0; i < docs; i++) {
    bulk.insert({a: i + "x".repeat(10 * 1000), arr: bigArr});
}
bulk.execute();

coll.createIndex({arr: 1, a: 1});

const serverStatus = testDB.serverStatus();
assert(serverStatus.hasOwnProperty("indexBulkBuilder"), "indexBuildBuilder section missing: " + tojson(serverStatus));

const section = serverStatus.indexBulkBuilder;
print("Index build stats", tojson(section));

const numSpills = section.spilledRanges;
assert.gt(numSpills, 0, tojson(section));

// Ensure the uncompressed memory usage per spill does not exceed the limit.
assert.between(0, section.bytesSpilledUncompressed / numSpills, maxMemUsageMB * 1024 * 1024, tojson(section));

replSet.stopSet();
