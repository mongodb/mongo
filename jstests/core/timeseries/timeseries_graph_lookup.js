/**
 * Verifies that time-series collections work as expected with $graphLookup.
 *
 * @tags: [
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
    const hostIdFieldName = "hostid";
    const nonTimeseriesCollOption = null;
    const timeseriesCollOption = {
        timeseries: {timeField: timeFieldName, metaField: hostIdFieldName}
    };
    const numHosts = 10;
    const numDocs = 200;

    Random.setRandomSeed();
    const hosts = TimeseriesTest.generateHosts(numHosts);

    let testFunc = function(collAOption, collBOption) {
        // Prepares two collections. Each collection can be either a time-series or a non
        // time-series collection, depending on collAOption/collBOption.
        const collA = testDB.getCollection("a");
        const collB = testDB.getCollection("b");
        collA.drop();
        collB.drop();
        assert.commandWorked(testDB.createCollection(collA.getName(), collAOption));
        assert.commandWorked(testDB.createCollection(collB.getName(), collBOption));
        let entryCountPerHost = new Array(numHosts).fill(0);
        let entryCountOver80AndExistsIdlePerHost = new Array(numHosts).fill(0);

        // Inserts into collA, one entry per host.
        for (let i = 0; i < numHosts; i++) {
            let host = hosts[i];
            assert.commandWorked(insert(collA, {time: ISODate(), hostid: host.tags.hostid}));
        }

        // Inserts some random documents to collB. The 'idle' measurement is inserted only when
        // usage is odd.
        for (let i = 0; i < numDocs; i++) {
            let host = TimeseriesTest.getRandomElem(hosts);
            let usage = TimeseriesTest.getRandomUsage();
            if (usage % 2) {
                assert.commandWorked(insert(
                    collB,
                    {time: ISODate(), hostid: host.tags.hostid, cpu: usage, idle: 100 - usage}));
            } else {
                assert.commandWorked(
                    insert(collB, {time: ISODate(), hostid: host.tags.hostid, cpu: usage}));
            }

            // These counts are to test metaField match.
            entryCountPerHost[host.tags.hostid]++;

            // These counts are to test measurement fields match which are specified by
            // $graphLookup's restrictSearchWithMatch.
            if (usage > 80 && usage % 2) {
                entryCountOver80AndExistsIdlePerHost[host.tags.hostid]++;
            }
        }

        // Verifies that a meta field "hostid" works with $graphLookup.
        let results = collA.aggregate([
                {
                    $graphLookup: {
                        from: collB.getName(),
                        startWith: "$hostid",
                        connectFromField: "hostid",
                        connectToField: "hostid",
                        as: "matchedB",
                        maxDepth: 0
                    }
                }, {
                    $project: {
                        _id: 0,
                        hostid: 1,
                        matchedB: {
                            $size: "$matchedB"
                        }
                    }
                },
                {$sort: {hostid: 1}}
            ]).toArray();

        assert.eq(numHosts, results.length, results);

        for (let i = 0; i < numHosts; i++) {
            assert.eq({hostid: i, matchedB: entryCountPerHost[i]}, results[i], results);
        }

        // Verifies that measurement fields "cpu" and "idle" work with $graphLookup as expected.
        results = collA.aggregate([
            {
                $graphLookup: {
                    from: collB.getName(),
                    startWith: "$hostid",
                    connectFromField: "hostid",
                    connectToField: "hostid",
                    as: "matchedB",
                    maxDepth: 0,
                    restrictSearchWithMatch: {
                        cpu: {$gt: 80},             // Tests measurement "cpu".
                        idle: {$exists: true}       // Tests the existence of measurement "idle".
                    }
                }
            }, {
                $project: {
                    _id: 0,
                    hostid: 1,
                    matchedB: {
                        $size: "$matchedB"
                    }
                }
            },
            {$sort: {hostid: 1}}
        ]).toArray();

        assert.eq(numHosts, results.length, results);

        for (let i = 0; i < numHosts; i++) {
            let expectedCount = entryCountOver80AndExistsIdlePerHost[i];
            assert.eq({hostid: i, matchedB: expectedCount},
                      results[i],
                      entryCountOver80AndExistsIdlePerHost);
        }
    };

    // Tests case #1: collA: non time-series, collB: time-series
    var collAOption = nonTimeseriesCollOption;
    var collBOption = timeseriesCollOption;
    testFunc(collAOption, collBOption);

    // Tests case #2: collA: time-series, collB: non time-series
    collAOption = timeseriesCollOption;
    collBOption = nonTimeseriesCollOption;
    testFunc(collAOption, collBOption);

    // Tests case #3: collA: time-series, collB: time-series
    collAOption = timeseriesCollOption;
    collBOption = timeseriesCollOption;
    testFunc(collAOption, collBOption);
});
})();
