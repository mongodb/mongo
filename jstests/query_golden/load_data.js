/**
 * Tests loading of test data stored in a JS file.
 * @tags: [
 *   requires_cqf,
 * ]
 */

(function() {
load("jstests/libs/load_ce_test_data.js");

const dbName = 'ce_accuracy_test';
const dataDir = 'jstests/query_golden/libs/data/';
const testDB = db.getSiblingDB(dbName);

// The schema script creates a global variable dbMetadata that holds a description of all
// collections.
load(`${dataDir}${dbName}.schema`);
print(`Metadata: ${tojson(dbMetadata)}\n`);

// This load command will create a variable named dataSet that contains all the data.
load(`${dataDir}${dbName}.data`);

print(`Loading ${dataSet.length} collections.\n`);
loadJSONDataset(testDB, dataSet, dbMetadata);

for (const collMetadata of dbMetadata) {
    const collName = collMetadata.collectionName;
    const coll = testDB[collName];
    const expectedCard = collMetadata.cardinality;
    const actualCard = coll.find().itcount();
    print(`\nTesting collection ${collName}\n`);
    print(`Indexes: ${tojson(coll.getIndexes())}\n`);
    print(`Expected cardinality: ${expectedCard}\n`);
    print(`Actual cardinality: ${actualCard}\n`);
    assert.eq(expectedCard, actualCard);
    print('Histogram:\n');
    let statsColl = testDB.system.statistics[collName];
    statsColl.find().forEach(function(doc) {
        jsTestLog(doc);
    });
}
})();
