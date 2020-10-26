/**
 * Tests inserting sample data into the time-series buckets collection.
 * This test is for the simple case of only one measurement per bucket.
 * @tags: [
 *     requires_fcv_49,
 * ]
 */
(function() {
"use strict";

load("jstests/core/time_series/libs/time_series.js");

if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
    jsTestLog("Skipping test because the time-series collection feature flag is disabled");
    return;
}

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());

const coll = testDB.getCollection('t');
const bucketsColl = testDB.getCollection('system.buckets.' + coll.getName());

assert.commandWorked(testDB.createCollection(coll.getName(), {timeseries: {timeField: 'time'}}));
assert.contains(bucketsColl.getName(), testDB.getCollectionNames());

Random.setRandomSeed();
const numHosts = 10;
const hosts = TimeseriesTest.generateHosts(numHosts);

for (let i = 0; i < 100; i++) {
    const host = TimeseriesTest.getRandomElem(hosts);
    TimeseriesTest.updateUsages(host.fields);

    const t = ISODate();
    const oid = ObjectId();
    const doc = {
        control: {
            version: 1,
            min: {
                _id: oid,
                time: t,
                usage_guest: host.fields.usage_guest,
                usage_guest_nice: host.fields.usage_guest_nice,
                // ...
                usage_user: host.fields.usage_user,
            },
            max: {
                _id: oid,
                time: t,
                usage_guest: host.fields.usage_guest,
                usage_guest_nice: host.fields.usage_guest_nice,
                // ...
                usage_user: host.fields.usage_user,
            },
        },
        meta: host.tags,
        data: {
            _id: {
                0: oid,
            },
            usage_guest: {
                0: host.fields.usage_guest,
            },
            usage_guest_nice: {
                0: host.fields.usage_guest_nice,
            },
            // ...
            usage_user: {
                0: host.fields.usage_user,
            },
        },
    };

    jsTestLog('Inserting doc into buckets collection: ' + i + ': ' + tojson(doc));
    assert.commandWorked(bucketsColl.insert(doc));
}
})();
