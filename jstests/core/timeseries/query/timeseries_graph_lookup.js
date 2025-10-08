/**
 * Verifies that time-series collections work as expected with $graphLookup.
 *
 * @tags: [
 *   # We need a timeseries collection.
 *   requires_timeseries,
 *   references_foreign_collection,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());
    const timeFieldName = "time";
    const hostIdFieldName = "hostid";
    const nonTimeseriesCollOption = null;
    const timeseriesCollOption = {
        timeseries: {timeField: timeFieldName, metaField: hostIdFieldName},
    };
    const numHosts = 10;
    const numDocs = 200;

    Random.setRandomSeed();
    const hosts = TimeseriesTest.generateHosts(numHosts);

    let testFunc = function (collAOption, collBOption) {
        // Prepares two collections. Each collection can be either a time-series or a non
        // time-series collection, depending on collAOption/collBOption.
        const collA = testDB.getCollection("a");
        const collB = testDB.getCollection("b");
        collA.drop();
        collB.drop();
        assert.commandWorked(testDB.createCollection(collA.getName(), collAOption));
        assert.commandWorked(testDB.createCollection(collB.getName(), collBOption));
        let entryCountPerHost = new Array(numHosts).fill(0);
        let entryCountPerInfo = new Array(numHosts).fill(0);
        let entryCountOver80AndExistsIdlePerHost = new Array(numHosts).fill(0);

        // Inserts into collA, one entry per host.
        for (let i = 0; i < numHosts; i++) {
            let host = hosts[i];
            assert.commandWorked(
                insert(collA, {time: ISODate(), hostid: host.tags.hostid, info: host.tags.hostid * 11}),
            );
        }

        // Inserts some random documents to collB. The 'idle' measurement is inserted only when
        // usage is odd. 'info' field is used to match the documents on a non-meta field.
        for (let i = 0; i < numDocs; i++) {
            let host = TimeseriesTest.getRandomElem(hosts);
            let usage = TimeseriesTest.getRandomUsage();
            if (usage % 2) {
                assert.commandWorked(
                    insert(collB, {
                        time: ISODate(),
                        hostid: host.tags.hostid,
                        cpu: usage,
                        idle: 100 - usage,
                        info: host.tags.hostid * 11,
                    }),
                );
            } else {
                assert.commandWorked(
                    insert(collB, {time: ISODate(), hostid: host.tags.hostid, cpu: usage, info: host.tags.hostid - 11}),
                );
            }

            // These counts are to test metaField match.
            entryCountPerHost[host.tags.hostid]++;

            // These counts are to test measurement field match.
            if (usage % 2) {
                entryCountPerInfo[host.tags.hostid]++;
            }

            // These counts are to test measurement fields match which are specified by
            // $graphLookup's restrictSearchWithMatch.
            if (usage > 80 && usage % 2) {
                entryCountOver80AndExistsIdlePerHost[host.tags.hostid]++;
            }
        }

        // Verifies that a meta field "hostid" works with $graphLookup.
        let results = collA
            .aggregate([
                {
                    $graphLookup: {
                        from: collB.getName(),
                        startWith: "$hostid",
                        connectFromField: "hostid",
                        connectToField: "hostid",
                        as: "matchedB",
                        maxDepth: 0,
                    },
                },
                {
                    $project: {
                        _id: 0,
                        hostid: 1,
                        matchedB: {
                            $size: "$matchedB",
                        },
                    },
                },
                {$sort: {hostid: 1}},
            ])
            .toArray();

        let expected = Array.from({length: numHosts}, (_, i) => ({
            hostid: i,
            matchedB: entryCountPerHost[i],
        }));
        assertArrayEq({
            actual: results,
            expected: expected,
            extraErrorMsg: "unexpected results with $graphLookup on a metaField",
        });

        // Verifies that a measurement field "info" works with $graphLookup.
        results = collA
            .aggregate([
                {
                    $graphLookup: {
                        from: collB.getName(),
                        startWith: "$info",
                        connectFromField: "info",
                        connectToField: "info",
                        as: "matchedB",
                        maxDepth: 0,
                    },
                },
                {
                    $project: {
                        _id: 0,
                        hostid: 1,
                        info: 1,
                        matchedB: {
                            $size: "$matchedB",
                        },
                    },
                },
                {$sort: {hostid: 1}},
            ])
            .toArray();

        expected = Array.from({length: numHosts}, (_, i) => ({
            hostid: i,
            info: i * 11,
            matchedB: entryCountPerInfo[i],
        }));
        assertArrayEq({
            actual: results,
            expected: expected,
            extraErrorMsg: "unexpected results with $graphLookup on a measurement field",
        });

        // Verifies that measurement fields "cpu" and "idle" work with $graphLookup as expected.
        results = collA
            .aggregate([
                {
                    $graphLookup: {
                        from: collB.getName(),
                        startWith: "$hostid",
                        connectFromField: "hostid",
                        connectToField: "hostid",
                        as: "matchedB",
                        maxDepth: 0,
                        restrictSearchWithMatch: {
                            cpu: {$gt: 80}, // Tests measurement "cpu".
                            idle: {$exists: true}, // Tests the existence of measurement "idle".
                        },
                    },
                },
                {
                    $project: {
                        _id: 0,
                        hostid: 1,
                        matchedB: {
                            $size: "$matchedB",
                        },
                    },
                },
                {$sort: {hostid: 1}},
            ])
            .toArray();

        expected = Array.from({length: numHosts}, (_, i) => ({
            hostid: i,
            matchedB: entryCountOver80AndExistsIdlePerHost[i],
        }));
        assertArrayEq({
            actual: results,
            expected: expected,
            extraErrorMsg: "unexpected results with $graphLookup with 'restrictSearchWithMatch'",
        });
    };

    // Tests case #1: collA: non time-series, collB: time-series
    let collAOption = nonTimeseriesCollOption;
    let collBOption = timeseriesCollOption;
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
