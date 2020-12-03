/**
 * Inserts time-series data based on the TSBS document-per-event format.
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

const coll = db.getCollection(jsTestName());
Random.setRandomSeed();

const numHosts = 10;
const hosts = TimeseriesTest.generateHosts(numHosts);

for (let i = 0; i < 100; i++) {
    const host = TimeseriesTest.getRandomElem(hosts);
    TimeseriesTest.updateUsages(host.fields);

    assert.commandWorked(coll.insert({
        measurement: "cpu",
        time: ISODate(),
        fields: host.fields,
        tags: host.tags,
    }));
}
})();