/**
 * Tests that the indexOptionDefaults collection creation option is applied when creating indexes on
 * a time-series collection.
 *
 * @tags: [
 *   requires_persistence,
 *   requires_wiredtiger,
 * ]
 */
const conn = MongoRunner.runMongod();

const testDB = conn.getDB("test");
const coll = testDB.getCollection(jsTestName());

assert.commandFailedWithCode(
    testDB.createCollection(coll.getName(), {
        timeseries: {timeField: "tt", metaField: "mm"},
        indexOptionDefaults: {storageEngine: {wiredTiger: {configString: "invalid_option=xxx,"}}},
    }),
    ErrorCodes.BadValue,
);

// Sample wiredtiger configuration option from wt_index_option_defaults.js.
assert.commandWorked(
    testDB.createCollection(coll.getName(), {
        timeseries: {timeField: "tt", metaField: "mm"},
        indexOptionDefaults: {storageEngine: {wiredTiger: {configString: "split_pct=88,"}}},
    }),
);

assert.commandWorked(coll.insert({tt: ISODate(), mm: "aaa"}));
assert.commandWorked(coll.createIndex({mm: 1}));

const indexCreationString = coll.stats({indexDetails: true}).indexDetails.mm_1.creationString;
assert.neq(-1, indexCreationString.indexOf(",split_pct=88,"), indexCreationString);

MongoRunner.stopMongod(conn);
