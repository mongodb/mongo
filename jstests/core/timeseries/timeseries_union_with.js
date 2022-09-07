/**
 * Test that time-series bucket collections work as expected with $unionWith.
 *
 * @tags: [
 *   # This test depends on certain writes ending up in the same bucket. Stepdowns may result in
 *   # writes splitting between two primaries, and thus different buckets.
 *   does_not_support_stepdowns,
 *   # We need a timeseries collection.
 *   requires_timeseries,
 * ]
 */
(function() {
"use strict";

load("jstests/core/timeseries/libs/timeseries.js");

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const timeFieldName = "time";
    const tagFieldName = "tag";
    const collOptions = [null, {timeseries: {timeField: timeFieldName, metaField: tagFieldName}}];
    const numHosts = 10;
    const numDocs = 200;

    Random.setRandomSeed();
    const hosts = TimeseriesTest.generateHosts(numHosts);

    let testFunc = function(collAOption, collBOption) {
        // Prepare two time-series collections.
        const collA = testDB.getCollection("a");
        const collB = testDB.getCollection("b");
        collA.drop();
        collB.drop();
        assert.commandWorked(testDB.createCollection(collA.getName(), collAOption));
        assert.commandWorked(testDB.createCollection(collB.getName(), collBOption));
        let entryCountPerHost = new Array(numHosts).fill(0);
        let insertTimeseriesDoc = function(coll) {
            let host = TimeseriesTest.getRandomElem(hosts);
            assert.commandWorked(insert(coll, {
                measurement: "cpu",
                time: ISODate(),
                tags: host.tags,
            }));
            // Here we extract the hostId from "host.tags.hostname". It is expected that the
            // "host.tags.hostname" is in the form of 'host_<hostNum>'.
            return parseInt(host.tags.hostname.substring(5, host.tags.hostname.length));
        };
        for (let i = 0; i < numDocs; i++) {
            let hostId = insertTimeseriesDoc(collA);
            entryCountPerHost[hostId]++;
            hostId = insertTimeseriesDoc(collB);
            if (hostId % 2 == 0) {
                // Calculate the expected entry count per host. Later we will union collA entries
                // with the collB entries whose hostId is even.
                entryCountPerHost[hostId]++;
            }
        }

        const results =
            collA
                .aggregate([
                    {
                        $unionWith: {
                            coll: collB.getName(),
                            pipeline: [{
                                $match: {
                                    $expr: {
                                        $eq: [
                                            {
                                                $mod: [
                                                    {$toInt: {$substr: ["$tags.hostname", 5, -1]}},
                                                    2
                                                ]
                                            },
                                            0
                                        ]
                                    }
                                }
                            }]
                        }
                    },
                    {$group: {_id: "$tags.hostname", count: {$sum: 1}}},
                    {$sort: {_id: 1}}
                ])
                .toArray();
        assert.eq(numHosts, results.length, results);
        for (let i = 0; i < numHosts; i++) {
            assert.eq({_id: "host_" + i, count: entryCountPerHost[i]}, results[i], results);
        }
    };

    // Exhaust the combinations of non-time-series and time-series collections for $unionWith
    // parameters.
    collOptions.forEach(function(collAOption) {
        collOptions.forEach(function(collBOption) {
            if (collAOption == null && collBOption == null) {
                // Normal $unionWith call, both inner and outer collections are non-time-series
                // collections.
                return;
            }
            testFunc(collAOption, collBOption);
        });
    });
});
})();
