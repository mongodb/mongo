/**
 * Tests replicated truncates on a clustered collection.
 * Creates a clustered collection and inserts a number of documents.
 * Triggers a replicated truncate via applyOps that injects a truncateRange oplog entry.
 * Validates a consistent state between primary and secondary after the truncate.
 * Tests different combinations of start and end bounds for the truncate range.
 *
 * @tags: [
 *   requires_replication,
 *   requires_fcv_83,
 *   featureFlagUseReplicatedTruncatesForDeletions,
 * ]
 */
import {ReplSetTest} from "jstests/libs/replsettest.js";

const rst = new ReplSetTest({nodes: 2});
rst.startSet();
rst.initiate();
rst.awaitSecondaryNodes();

const primary = rst.getPrimary();
const dbName = "test";
const collName = "coll";
const testDB = primary.getDB(dbName);
const coll = testDB.getCollection(collName);
const secondary = rst.getSecondary();
const secondaryDB = secondary.getDB(dbName);
const secondaryColl = secondaryDB.getCollection(collName);

function setup(numToInsert) {
    jsTest.log.info("Creating clustered collection");
    assert.commandWorked(testDB.createCollection(collName, {clusteredIndex: {key: {_id: 1}, unique: true}}));

    jsTest.log.info(`Inserting ${numToInsert} documents`);
    let docs = [];
    for (let i = 1; i <= numToInsert; i++) {
        docs.push({_id: i});
    }
    assert.commandWorked(coll.insertMany(docs));

    jsTest.log.info("Waiting for replication after insert");
    rst.awaitReplication();
}

function doTest(startIndex, endIndex, originalDocs, docsBeforeTruncate) {
    const minId = originalDocs[startIndex]._id;
    const maxId = originalDocs[endIndex]._id;

    // Truncate the array
    const expectedDocs = docsBeforeTruncate.filter((item) => item._id < minId || item._id > maxId);
    const recordsDeleted = docsBeforeTruncate.length - expectedDocs.length;

    jsTest.log.info(`Truncating ${recordsDeleted} documents from {_id: ${minId}} to {_id: ${maxId}}`);
    const applyOpsCmd = {
        applyOps: [
            {
                op: "c",
                ns: `${dbName}.$cmd`,
                o: {
                    "truncateRange": coll.getFullName(),
                    "minRecordId": originalDocs[startIndex].$recordId,
                    "maxRecordId": originalDocs[endIndex].$recordId,
                    "bytesDeleted": recordsDeleted, // just a placeholder
                    "docsDeleted": recordsDeleted,
                },
            },
        ],
    };
    jsTest.log.info(`Applying applyOps: ${tojson(applyOpsCmd)}`);
    assert.commandWorked(testDB.runCommand(applyOpsCmd));

    jsTest.log.info("Waiting for replication after truncate");
    rst.awaitReplication();

    // Validate the collection contents match expected after truncate
    const primaryDocs = coll.find().sort({_id: 1}).showRecordId().toArray();
    assert.eq(primaryDocs, expectedDocs, "Primary documents not as expected after truncate");

    const secondaryDocs = secondaryColl.find().sort({_id: 1}).showRecordId().toArray();
    assert.eq(secondaryDocs, expectedDocs, "Secondary documents not as expected after truncate");

    // Return docs for next iteration
    return expectedDocs;
}

// Insert 100 documents
const numToInsert = 100;
setup(numToInsert);
const docs = coll.find().sort({_id: 1}).showRecordId().toArray();
assert.eq(docs.length, numToInsert, `Unexpected number of documents after insert: ${tojson(docs)}`);

// Truncate the front
let docsAfterTest = doTest(0, 10, docs, docs);
// Truncate the middle
docsAfterTest = doTest(50, 60, docs, docsAfterTest);
// Truncate the end
docsAfterTest = doTest(90, 99, docs, docsAfterTest);
// Truncate everything
docsAfterTest = doTest(0, 99, docs, docsAfterTest);
// Truncate again
docsAfterTest = doTest(0, 99, docs, docsAfterTest);
assert.eq(docsAfterTest.length, 0);

rst.stopSet();
