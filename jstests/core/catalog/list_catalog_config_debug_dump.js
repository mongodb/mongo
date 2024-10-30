/*
 * Test basic functionality of configDebugDump flag in $listCatalog
 *
 * @tags: [
 *   requires_fcv_81,
 *   requires_getmore,
 * ]
 */

const myDBName = jsTestName();
const myDB = db.getSiblingDB(myDBName);
const config = db.getSiblingDB("config");
const admin = db.getSiblingDB("admin");
const dummyCollectionName = "dummyCollection";
const databasesCollectionName = "databases";

assert.commandWorked(myDB[dummyCollectionName].insertOne({x: 1}));
assert.commandWorked(myDB[databasesCollectionName].insertOne({x: 1}));

// In suites with tenantID, the test would replace a simple {$match: {db: myDBName}}
// with {$match: {db: tenantID + "_" + myDBName}}, hence the workaround with a regex.
const userDBlist =
    admin.aggregate([{$listCatalog: {}}, {$match: {db: {$regex: `^${myDBName}\$`}}}]).toArray();
assert.gte(userDBlist.length, 2);

let dummyFound = false;
let databasesFound = false;

// All entries of $listCatalog of any database other than config don't have a `configDebugDump`
// field.
for (let c of userDBlist) {
    assert(!c.hasOwnProperty("configDebugDump"));

    if (c.name == dummyCollectionName) {
        dummyFound = true;
    }
    if (c.name == databasesCollectionName) {
        databasesFound = true;
    }
}

assert(dummyFound);
assert(databasesFound);

assert.commandWorked(config[dummyCollectionName].insertOne({x: 1}));
// config.databases is a collection for which `info.configDebugDump` is true. Check if it exists (we
// might be connected to a sharded cluster), otherwise create it.
if (!config[databasesCollectionName].exists()) {
    assert.commandWorked(config[databasesCollectionName].insertOne({x: 1}));
}

const configList = admin.aggregate([{$listCatalog: {}}, {$match: {db: "config"}}]).toArray();

// All entries of $listCatalog of the config database have a `configDebugDump` field, which is a
// boolean. Check also a couple collections for the correct value.
for (let c of configList) {
    assert(c.hasOwnProperty("configDebugDump"));
    assert.eq("boolean", typeof (c.configDebugDump));

    if (c.name == dummyCollectionName) {
        assert.eq(false, c.configDebugDump);
    }
    if (c.name == databasesCollectionName) {
        assert.eq(true, c.configDebugDump);
    }
}

// When run against mongoS, $listCatalog will return the aggregated catalog of all shards but not
// the config server. For this reason, run a non-collectionless $listCatalog against
// config.databases and config.dummyCollection. This also just tests the non-collectionless case.

function testSingleCollectionAggregate(array, expectedValue) {
    assert.eq(1, array.length);
    assert(array[0].hasOwnProperty("configDebugDump"));
    assert.eq("boolean", typeof (array[0].configDebugDump));
    assert.eq(expectedValue, array[0].configDebugDump);
}

const dummyList = config[dummyCollectionName].aggregate([{$listCatalog: {}}]).toArray();
const databasesList = config[databasesCollectionName].aggregate([{$listCatalog: {}}]).toArray();

testSingleCollectionAggregate(dummyList, false);
testSingleCollectionAggregate(databasesList, true);
