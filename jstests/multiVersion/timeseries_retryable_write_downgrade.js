/**
 * Tests that config.transactions entries referencing oplog entries for time-series inserts, which
 * can't be parsed on versions older than 4.9 if they contain multiple statement ids, are removed on
 * FCV downgrade.
 *
 * TODO (SERVER-56171): Remove this test once 5.0 is last-lts.
 */
(function() {
'use strict';

load('jstests/core/timeseries/libs/timeseries.js');
load("jstests/multiVersion/libs/multi_rs.js");

const replTest = new ReplSetTest({nodes: 2});
replTest.startSet();
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

const configTransactions = primary.getDB('config')['transactions'];
assert.eq(configTransactions.find().toArray().length, 1);

assert(coll.drop());
assert.commandWorked(primary.adminCommand({setFeatureCompatibilityVersion: lastLTSFCV}));

// Downgrading the FCV removes the config.transactions entry referencing an oplog entry for a
// time-series insert.
assert.eq(configTransactions.find().toArray().length, 0);

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