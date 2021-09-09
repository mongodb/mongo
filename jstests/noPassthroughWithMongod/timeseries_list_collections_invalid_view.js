/**
 * Tests the behavior of listCollections in the presence of both a time-series collection and an
 * invalid view definition.
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     does_not_support_transactions,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 * ]
 */
(function() {
'use strict';

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const timeFieldName = 'time';
const coll = testDB.getCollection('t');

assert.commandWorked(
    testDB.createCollection(coll.getName(), {timeseries: {timeField: timeFieldName}}));
assert.commandWorked(testDB.adminCommand({
    applyOps: [
        {op: 'i', ns: testDB.getName() + '.system.views', o: {_id: 'invalid', pipeline: 'invalid'}}
    ]
}));

assert.commandFailedWithCode(testDB.runCommand({listCollections: 1}),
                             ErrorCodes.InvalidViewDefinition);
assert.commandFailedWithCode(testDB.runCommand({listCollections: 1, filter: {type: 'timeseries'}}),
                             ErrorCodes.InvalidViewDefinition);
assert.commandFailedWithCode(
    testDB.runCommand({listCollections: 1, filter: {name: coll.getName()}}),
    ErrorCodes.InvalidViewDefinition);

// TODO (SERVER-25493): Change filter to {type: 'collection'}.
const collections =
    assert
        .commandWorked(testDB.runCommand(
            {listCollections: 1, filter: {$or: [{type: 'collection'}, {type: {$exists: false}}]}}))
        .cursor.firstBatch;
jsTestLog('Checking listCollections result: ' + tojson(collections));
assert.eq(collections.length, 2);
assert(collections.find(entry => entry.name === 'system.views'));
assert(collections.find(entry => entry.name === 'system.buckets.' + coll.getName()));

// Users are prohibited from dropping system.views when there are time-series collections present.
// However, this restriction isn't in place on earlier versions and its possible for users to
// upgrade to this version with a dropped system.views collection while having time-series
// collections present. This allows us to continue to test the behaviour of this scenario.
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "allowSystemViewsDrop", mode: "alwaysOn"}));
assert(testDB.system.views.drop());
assert.commandWorked(
    testDB.adminCommand({configureFailPoint: "allowSystemViewsDrop", mode: "off"}));
})();
