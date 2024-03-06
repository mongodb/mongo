/**
 * Tests profiler docs validation the run_validate_direct_secondary_reads.js.
 *
 * @tags: [ requires_persistence ]
 */

import {extractUUIDFromObject} from "jstests/libs/uuid_util.js";
import {
    getTotalNumProfilerDocs,
    validateProfilerCollections
} from "jstests/noPassthrough/rs_endpoint/lib/validate_direct_secondary_reads.js";

function enableProfiling(rst, dbName) {
    rst.nodes.forEach(node => {
        assert.commandWorked(node.getDB(dbName).setProfilingLevel(2));
    });
}

const rst = new ReplSetTest({
    nodes: [
        {},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}},
        {rsConfig: {priority: 0}}
    ]
});
rst.startSet();
rst.initiate();

const primary = rst.getPrimary();
const secondaries = rst.getSecondaries();

const hostColl = primary.getDB("config").connectDirectlyToSecondaries.hosts;
const hostDocs = [
    {host: primary.host, isPrimary: true},
    {host: secondaries[0].host, isSecondary: true, comment: extractUUIDFromObject(UUID())},
    {host: secondaries[1].host, isSecondary: true, isExcluded: true},
    {host: secondaries[2].host, isSecondary: true, comment: extractUUIDFromObject(UUID())},
    {host: secondaries[3].host, isSecondary: true, comment: extractUUIDFromObject(UUID())}
];
assert.commandWorked(hostColl.insert(hostDocs));

const dbName = "testDb";
const collName = "testColl";
const primaryTestDB = primary.getDB(dbName);
const secondary0TestDB = secondaries[0].getDB(dbName);
const secondary1TestDB = secondaries[1].getDB(dbName);  // excluded from reading.
const secondary2TestDB = secondaries[2].getDB(dbName);
const secondary3TestDB = secondaries[3].getDB(dbName);

enableProfiling(rst, "config");
enableProfiling(rst, "local");
enableProfiling(rst, "admin");

{
    jsTest.log("Testing no reads in user database");
    const numProfilerDocsPerHost = {};
    validateProfilerCollections(hostDocs[0], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[1], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[2], hostDocs, numProfilerDocsPerHost);
    assert.eq(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);
}

{
    jsTest.log("Testing no unexpected reads in user database");
    enableProfiling(rst, dbName);
    assert.commandWorked(primaryTestDB.runCommand({insert: collName, documents: [{x: 1}]}));
    assert.commandWorked(secondary0TestDB.runCommand({find: collName, filter: {}}));
    assert.commandWorked(secondary1TestDB.runCommand({find: collName, filter: {}}));
    assert.commandWorked(secondary2TestDB.runCommand({find: collName, filter: {}}));
    assert.commandWorked(secondary3TestDB.runCommand({find: collName, filter: {}}));
    assert.commandWorked(
        secondary0TestDB.runCommand({find: collName, filter: {}, comment: hostDocs[1].comment}));
    assert.commandWorked(
        secondary2TestDB.runCommand({find: collName, filter: {}, comment: hostDocs[3].comment}));
    assert.commandWorked(
        secondary3TestDB.runCommand({find: collName, filter: {}, comment: hostDocs[4].comment}));
    rst.awaitReplication();

    const numProfilerDocsPerHost = {};
    validateProfilerCollections(hostDocs[0], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[1], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[2], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[3], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[4], hostDocs, numProfilerDocsPerHost);
    assert.gt(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);

    assert.commandWorked(primaryTestDB.runCommand({dropDatabase: 1}));
}

{
    jsTest.log("Testing unexpected reads in user database on non-excluded secondary");
    enableProfiling(rst, dbName);
    assert.commandWorked(primaryTestDB.runCommand({insert: collName, documents: [{x: 1}]}));
    assert.commandWorked(
        secondary0TestDB.runCommand({find: collName, filter: {}, comment: hostDocs[3].comment}));
    rst.awaitReplication();

    const numProfilerDocsPerHost = {};
    validateProfilerCollections(hostDocs[0], hostDocs, numProfilerDocsPerHost);
    assert.throws(() => validateProfilerCollections(hostDocs[1], hostDocs, numProfilerDocsPerHost));
    validateProfilerCollections(hostDocs[2], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[3], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[4], hostDocs, numProfilerDocsPerHost);
    assert.gt(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);

    assert.commandWorked(primaryTestDB.runCommand({dropDatabase: 1}));
}

{
    jsTest.log("Testing unexpected reads in user database on excluded secondary");
    enableProfiling(rst, dbName);
    assert.commandWorked(primaryTestDB.runCommand({insert: collName, documents: [{x: 1}]}));
    assert.commandWorked(
        secondary1TestDB.runCommand({find: collName, filter: {}, comment: hostDocs[3].comment}));
    rst.awaitReplication();

    const numProfilerDocsPerHost = {};
    validateProfilerCollections(hostDocs[0], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[1], hostDocs, numProfilerDocsPerHost);
    assert.throws(() => validateProfilerCollections(hostDocs[2], hostDocs, numProfilerDocsPerHost));
    validateProfilerCollections(hostDocs[3], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[4], hostDocs, numProfilerDocsPerHost);
    assert.gt(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);

    assert.commandWorked(primaryTestDB.runCommand({dropDatabase: 1}));
}

{
    jsTest.log("Testing reads in config database on primary");
    assert.commandWorked(primary.getDB("config").runCommand(
        {find: "chunks", filter: {}, comment: hostDocs[3].comment}));

    const numProfilerDocsPerHost = {};
    validateProfilerCollections(hostDocs[0], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[1], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[2], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[3], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[4], hostDocs, numProfilerDocsPerHost);
    assert.gt(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);
}

{
    jsTest.log("Testing writes triggered by reads on a non-excluded secondary");
    enableProfiling(rst, dbName);
    const comment = hostDocs[1].comment;
    assert.commandWorked(primaryTestDB.runCommand({create: collName, comment}));
    assert.commandWorked(primaryTestDB.runCommand(
        {createIndexes: collName, indexes: [{key: {x: 1}, name: "x_1"}], comment}));
    assert.commandWorked(primaryTestDB.runCommand({dropIndexes: collName, index: {x: 1}, comment}));
    assert.commandWorked(
        primaryTestDB.runCommand({insert: collName, documents: [{x: 1}, {x: 2}], comment}));
    assert.commandWorked(primaryTestDB.runCommand(
        {update: collName, updates: [{q: {x: 1}, u: {$set: {x: -1}}}], comment}));
    assert.commandWorked(
        primaryTestDB.runCommand({delete: collName, deletes: [{q: {x: 2}, limit: 1}], comment}));
    assert.commandWorked(primaryTestDB.runCommand(
        {findAndModify: collName, query: {x: 1}, update: {$set: {z: 1}}, comment}));
    assert.commandWorked(primaryTestDB.runCommand({drop: collName, comment}));
    rst.awaitReplication();

    const numProfilerDocsPerHost = {};
    validateProfilerCollections(hostDocs[0], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[1], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[2], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[3], hostDocs, numProfilerDocsPerHost);
    validateProfilerCollections(hostDocs[4], hostDocs, numProfilerDocsPerHost);
    assert.gt(getTotalNumProfilerDocs(numProfilerDocsPerHost), 0);

    assert.commandWorked(primaryTestDB.runCommand({dropDatabase: 1}));
}

rst.stopSet();
