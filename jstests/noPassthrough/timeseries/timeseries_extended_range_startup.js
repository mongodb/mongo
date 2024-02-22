/**
 * Tests that time-series collection that require extended range support are properly recognized
 * during startup recovery.
 * @tags: [
 *   requires_replication,
 *   # The primary is restarted and must retain its data.
 *   requires_persistence,
 * ]
 */
const getExtendedRangeCount = (db) => {
    return assert.commandWorked(db.adminCommand({serverStatus: 1}))
        .catalogStats.timeseriesExtendedRange;
};

const rst = new ReplSetTest({name: jsTest.name(), nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = "testDB";
const primaryDB = primary.getDB(dbName);

assert.eq(undefined, getExtendedRangeCount(primary));
assert.eq(undefined, getExtendedRangeCount(secondary));

assert.commandWorked(primaryDB.createCollection("standard", {timeseries: {timeField: "time"}}));
assert.commandWorked(primaryDB.createCollection("extended", {timeseries: {timeField: "time"}}));
assert.commandWorked(
    primaryDB.standard.insert({time: ISODate("1980-01-01T00:00:00.000Z")}, {w: 2}));
assert.commandWorked(
    primaryDB.extended.insert({time: ISODate("2040-01-01T00:00:00.000Z")}, {w: 2}));

// Make sure the collections got flagged properly during the initial write.
assert(checkLog.checkContainsWithCountJson(
    primary, 6679402, {"nss": "testDB.standard", "timeField": "time"}, 0));
assert(checkLog.checkContainsWithCountJson(
    secondary, 6679402, {"nss": "testDB.standard", "timeField": "time"}, 0));
assert(checkLog.checkContainsWithCountJson(
    primary, 6679402, {"nss": "testDB.extended", "timeField": "time"}, 1));
assert(checkLog.checkContainsWithCountJson(
    secondary, 6679402, {"nss": "testDB.extended", "timeField": "time"}, 1));

assert.eq(1, getExtendedRangeCount(primary));
assert.eq(1, getExtendedRangeCount(secondary));

rst.restart(primary, {skipValidation: true});
rst.waitForState(primary, ReplSetTest.State.SECONDARY);

assert.eq(1, primaryDB.standard.count());
assert.eq(1, primaryDB.extended.count());

// Make sure the collections get flagged properly again after startup.
assert.eq(1, getExtendedRangeCount(primary));

// As of SERVER-86451, time-series inconsistencies detected during validation
// will error in testing, instead of being warnings. In this case,
// validation on shutdown would fail, where before only a warning would be thrown.
// TODO SERVER-87065: Look into re-enabling validation on shutdown.
rst.stopSet(null, false, {skipValidation: true});
