/*
 * Test basic functionality of configDebugDump flag in listCollections
 *
 * @tags: [
 *   requires_fcv_81,
 *   requires_getmore,
 * ]
 */

const config = db.getSiblingDB("config");
const dummyCollectionName = "dummyCollection";
const databasesCollectionName = "databases";

assert.commandWorked(db[dummyCollectionName].insertOne({x: 1}));
assert.commandWorked(db[databasesCollectionName].insertOne({x: 1}));

const resultUserDB = db.runCommand({listCollections: 1});
assert.commandWorked(resultUserDB);
const userDBlist = (new DBCommandCursor(db, resultUserDB)).toArray();
assert.gte(userDBlist.length, 2);

let dummyFound = false;
let databasesFound = false;

// All entries of listCollections of any database other than config don't have a
// `info.configDebugDump` field.
for (let c of userDBlist) {
    assert(!c.hasOwnProperty("info") || !c.info.hasOwnProperty("configDebugDump"));

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

const result = config.runCommand({listCollections: 1});
assert.commandWorked(result);
const configList = (new DBCommandCursor(config, result)).toArray();

dummyFound = false;
databasesFound = false;

// All entries of listCollections of the config database have a `info.configDebugDump` field, which
// is a boolean. Check also a couple collections for the correct value.
for (let c of configList) {
    assert(c.hasOwnProperty("info"));
    assert(c.info.hasOwnProperty("configDebugDump"));
    assert.eq("boolean", typeof (c.info.configDebugDump));

    if (c.name == dummyCollectionName) {
        assert.eq(false, c.info.configDebugDump);
        dummyFound = true;
    }
    if (c.name == databasesCollectionName) {
        assert.eq(true, c.info.configDebugDump);
        databasesFound = true;
    }
}

assert(dummyFound);
assert(databasesFound);
