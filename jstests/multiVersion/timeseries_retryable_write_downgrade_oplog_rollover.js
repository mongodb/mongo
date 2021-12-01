/**
 * Tests that config.transactions entries referencing oplog entries that have fallen off the oplog
 * are ignored when removing retryable time-series entries from config.transactions on FCV
 * downgrade.
 *
 * TODO (SERVER-56171): Remove this test once 5.0 is last-lts.
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load("jstests/multiVersion/libs/multi_rs.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet({oplogSize: 1});  // Use a 1MB oplog.
replTest.initiate();

const primary = replTest.getPrimary();

if (!TimeseriesTest.timeseriesCollectionsEnabled(primary)) {
    jsTestLog('Skipping test because the time-series collection feature flag is disabled');
    replTest.stopSet();
    return;
}

const testDB = primary.startSession({retryWrites: true}).getDatabase('test');
const coll = testDB[jsTestName()];

const timeFieldName = 'time';

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));

// Insert two documents together which go into the same time-series bucket, so that the oplog entry
// has multiple statement ids.
assert.commandWorked(
    coll.insert([{_id: 0, [timeFieldName]: ISODate()}, {_id: 1, [timeFieldName]: ISODate()}],
                {ordered: false}));

// Wait for the oplog entry of the retryable time-series insert to fall off the oplog.
assert.soonNoExcept(() => {
    assert.commandWorked(primary.getDB(testDB.getName())[coll.getName() + '_rollover'].insert(
        {a: 'a'.repeat(1024 * 1024)}));
    return primary.getDB('local')
               .oplog.rs.find({op: 'i', ns: testDB.getName() + '.system.buckets.' + coll.getName()})
               .itcount() === 0;
});

const configTransactions = primary.getDB('config')['transactions'];
assert.eq(configTransactions.find().toArray().length, 1);

assert(coll.drop());
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// The oplog entry no longer exists, so the config.transactions entry is not removed.
assert.eq(configTransactions.find().toArray().length, 1);

replTest.upgradeSet({binVersion: 'last-lts'});
replTest.awaitNodesAgreeOnPrimary();
if (replTest.getPrimary().port !== primary.port) {
    replTest.stepUp(replTest.getSecondary());
}

// Another insert using the same session can succeed since it doesn't need to parse the oplog entry
// with multiple statement ids, which isn't supported on versions older than 4.9.
assert.commandWorked(coll.insert({_id: 0}));

replTest.stopSet();
})();
