/**
 * Test that time-series bucket collections work as expected with $lookup.
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

    let
        testFunc = function(collAOption, collBOption) {
            // Prepare two time-series collections.
            const collA = testDB.getCollection("a");
            const collB = testDB.getCollection("b");
            collA.drop();
            collB.drop();
            assert.commandWorked(testDB.createCollection(collA.getName(), collAOption));
            assert.commandWorked(testDB.createCollection(collB.getName(), collBOption));

            let entryCountPerHost = new Array(numHosts).fill(0);

            // Insert into collA, one entry per host.
            for (let i = 0; i < numHosts; i++) {
                let host = hosts[i];
                assert.commandWorked(insert(collA, {
                    measurement: "cpu",
                    time: ISODate(),
                    tags: host.tags,
                }));
            }

            // Insert some random documents to collB.
            for (let i = 0; i < numDocs; i++) {
                let host = TimeseriesTest.getRandomElem(hosts);
                assert.commandWorked(insert(collB, {
                    measurement: "cpu",
                    time: ISODate(),
                    tags: host.tags,
                }));
                // Here we extract the hostId from "host.tags.hostname". It is expected that the
                // "host.tags.hostname" is in the form of 'host_<hostNum>'.
                entryCountPerHost[parseInt(
                    host.tags.hostname.substring(5, host.tags.hostname.length))]++;
            }

            // Equality Match
            let results = collA.aggregate([
                {
                    $lookup: {
                        from: collB.getName(),
                        localField: "tags.hostname",
                        foreignField: "tags.hostname",
                        as: "matchedB"
                    }
                }, {
                    $project: {
                        _id: 0,
                        host: "$tags.hostname",
                        matchedB: {
                            $size: "$matchedB"
                        }
                    }
                },
                {$sort: {host: 1}}
            ]).toArray();
            assert.eq(numHosts, results.length, results);
            for (let i = 0; i < numHosts; i++) {
                assert.eq({host: "host_" + i, matchedB: entryCountPerHost[i]}, results[i], results);
            }

            // Equality Match With Let (uncorrelated)
            // Make sure injected $sequentialDocumentCache (right after unpack bucket stage)
            // in the inner pipeline is removed.
            results = collA.aggregate([
            {
                $lookup: {
                    from: collB.getName(),
                    let: {"outer_hostname": "$tags.hostname"},
                    pipeline: [
                        // $match will be pushed before unpack bucket stage
                        {$match: {$expr: {$eq: ["$$outer_hostname", hosts[0].tags.hostname]}}},
                    ],
                    as: "matchedB"
                }
            }, {
                $project: {
                    _id: 0,
                    host: "$tags.hostname",
                    matchedB: {
                        $size: "$matchedB"
                    }
                }
            },
            {$sort: {host: 1}}
        ]).toArray();
            assert.eq(numHosts, results.length, results);
            for (let i = 0; i < numHosts; i++) {
                const matched = i === 0 ? numDocs : 0;
                assert.eq({host: "host_" + i, matchedB: matched}, results[i], results);
            }

            // Equality Match With Let (uncorrelated)
            // Make sure injected $sequentialDocumentCache in the inner pipeline is removed.
            // $sequentialDocumentCache is not located right after unpack bucket stage.
            results = collA.aggregate([
                    {
                        $lookup: {
                            from: collB.getName(),
                            let: {"outer_hostname": "$tags.hostname"},
                            pipeline: [
                                {$match: {$expr: {$eq: ["$$outer_hostname", hosts[0].tags.hostname]}}},
                                {$set: {foo: {$const: 123}}},  // uncorrelated
                            ],
                            as: "matchedB"
                        }
                    }, {
                        $project: {
                            _id: 0,
                            host: "$tags.hostname",
                            matchedB: {
                                $size: "$matchedB"
                            }
                        }
                    },
                    {$sort: {host: 1}}
                ]).toArray();
            assert.eq(numHosts, results.length, results);
            for (let i = 0; i < numHosts; i++) {
                const matched = i === 0 ? numDocs : 0;
                assert.eq({host: "host_" + i, matchedB: matched}, results[i], results);
            }

            // Equality Match With Let (correlated, no $match re-order)
            // Make sure injected $sequentialDocumentCache in the inner pipeline is removed.
            // $sequentialDocumentCache is located at the very end of pipeline.
            results = collA.aggregate([
            {
                $lookup: {
                    from: collB.getName(),
                    let: {"outer_hostname": "$tags.hostname"},
                    pipeline: [
                        {$match: {$expr: {$eq: ["$$outer_hostname", hosts[0].tags.hostname]}}},
                        {$set: {foo: "$$outer_hostname"}},  // correlated
                    ],
                    as: "matchedB"
                }
            }, {
                $project: {
                    _id: 0,
                    host: "$tags.hostname",
                    matchedB: {
                        $size: "$matchedB"
                    }
                }
            },
            {$sort: {host: 1}}
        ]).toArray();
            assert.eq(numHosts, results.length, results);
            for (let i = 0; i < numHosts; i++) {
                const matched = i === 0 ? numDocs : 0;
                assert.eq({host: "host_" + i, matchedB: matched}, results[i], results);
            }

            // Unequal joins
            results = collA.aggregate([
            {
                $lookup: {
                    from: collB.getName(),
                    let: {
                        hostId: {$toInt: {$substr: ["$tags.hostname", 5, -1]}}
                    },
                    pipeline: [
                        {
                            $match: {
                                $expr: {
                                    $lte: [{$toInt: {$substr: ["$tags.hostname", 5, -1]}}, "$$hostId"]
                                }
                            },
                        }
                    ],
                    as: "matchedB"
                }
            }, {
                $project: {
                    _id: 0,
                    host: "$tags.hostname",
                    matchedB: {
                        $size: "$matchedB"
                    }
                }
            },
            {$sort: {host: 1}}
        ]).toArray();
            assert.eq(numHosts, results.length, results);
            let expectedCount = 0;
            for (let i = 0; i < numHosts; i++) {
                expectedCount += entryCountPerHost[i];
                assert.eq(
                    {host: "host_" + i, matchedB: expectedCount}, results[i], entryCountPerHost);
            }

            // $sequenceDocumentsCache might optimize out $internalUnpackBucket and cause a
            // crash on such query.
            results = collA.aggregate([
                {$lookup: {
                    from: collB.getName(),
                    as: 'docs',
                    let: {},
                    pipeline: [
                        {$sort: {_id: 1}}
                    ]
                }}
            ]).toArray();
            assert.eq(numHosts, results.length, results);
        };

    // Exhaust the combinations of non-time-series and time-series collections for inner and outer
    // $lookup collections.
    collOptions.forEach(function(collAOption) {
        collOptions.forEach(function(collBOption) {
            if (collAOption == null && collBOption == null) {
                // Normal $lookup call, both inner and outer collections are non-time-series
                // collections.
                return;
            }
            testFunc(collAOption, collBOption);
        });
    });
});
})();
