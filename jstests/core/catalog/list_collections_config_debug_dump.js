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
const dummyViewName = "dummyView";
const dummyTimeseriesName = "dummyTimeseries";
const databasesCollectionName = "databases";

assert.commandWorked(db[dummyCollectionName].insertOne({x: 1}));
assert.commandWorked(db[databasesCollectionName].insertOne({x: 1}));
assert.commandWorked(db.createCollection(dummyTimeseriesName, {timeseries: {timeField: "t"}}));
assert.commandWorked(db.createCollection(dummyViewName, {viewOn: dummyCollectionName}));

const resultUserDB = db.runCommand({listCollections: 1});
assert.commandWorked(resultUserDB);
const userDBlist = new DBCommandCursor(db, resultUserDB).toArray();
assert.gte(userDBlist.length, 4);

let dummyCollectionFound = false;
let dummyViewFound = false;
let dummyTimeseriesFound = false;
let databasesFound = false;

// All entries of listCollections of any database other than config don't have a
// `info.configDebugDump` field.
for (let c of userDBlist) {
    assert(
        !c.hasOwnProperty("info") || !c.info.hasOwnProperty("configDebugDump"),
        "Found the configDebugDump field for the listCollection entry on a non-config db. Entry : " + tojson(c),
    );

    if (c.name == dummyCollectionName) {
        dummyCollectionFound = true;
    }
    if (c.name == databasesCollectionName) {
        databasesFound = true;
    }
    if (c.name == dummyTimeseriesName) {
        dummyTimeseriesFound = true;
    }
    if (c.name == dummyViewName) {
        dummyViewFound = true;
    }
}

assert(dummyCollectionFound);
assert(dummyViewFound);
assert(dummyTimeseriesFound);
assert(databasesFound);

assert.commandWorked(config[dummyCollectionName].insertOne({x: 1}));
// config.databases is a collection for which `info.configDebugDump` is true. Check if it exists (we
// might be connected to a sharded cluster), otherwise create it.
if (!config[databasesCollectionName].exists()) {
    assert.commandWorked(config[databasesCollectionName].insertOne({x: 1}));
}
assert.commandWorked(config.createCollection(dummyTimeseriesName, {timeseries: {timeField: "t"}}));
assert.commandWorked(config.createCollection(dummyViewName, {viewOn: dummyCollectionName}));

const result = config.runCommand({listCollections: 1});
assert.commandWorked(result);
const configList = new DBCommandCursor(config, result).toArray();

dummyCollectionFound = false;
dummyViewFound = false;
dummyTimeseriesFound = false;
databasesFound = false;

// All entries of listCollections of the config database have a `info.configDebugDump` field, which
// is a boolean. Check also a couple collections for the correct value.
for (let c of configList) {
    assert(
        c.hasOwnProperty("info"),
        "Missing the info entry field for a listCollection entry on the config db. Entry : " + tojson(c),
    );
    assert(
        c.info.hasOwnProperty("configDebugDump"),
        "Missing the configDebugDump field for the listCollection entry on the config db. Entry : " + tojson(c),
    );
    assert.eq("boolean", typeof c.info.configDebugDump);

    if (c.name == dummyCollectionName) {
        assert.eq(false, c.info.configDebugDump);
        dummyCollectionFound = true;
    }
    if (c.name == databasesCollectionName) {
        assert.eq(true, c.info.configDebugDump);
        databasesFound = true;
    }
    if (c.name == dummyTimeseriesName) {
        assert.eq(false, c.info.configDebugDump);
        dummyTimeseriesFound = true;
    }
    if (c.name == dummyViewName) {
        assert.eq(false, c.info.configDebugDump);
        dummyViewFound = true;
    }
}

assert(dummyCollectionFound);
assert(dummyViewFound);
assert(dummyTimeseriesFound);
assert(databasesFound);
