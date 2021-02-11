/**
 * Test that time-series bucket collections work as expected with $lookup
 *
 * @tags: [
 *     assumes_against_mongod_not_mongos,
 *     assumes_no_implicit_collection_creation_after_drop,
 *     does_not_support_stepdowns,
 *     requires_fcv_49,
 *     requires_find_command,
 *     requires_getmore,
 *     sbe_incompatible,
 * ]
 */
(function () {
    "use strict";

    load("jstests/core/timeseries/libs/timeseries.js");

    if (!TimeseriesTest.timeseriesCollectionsEnabled(db.getMongo())) {
        jsTestLog("Skipping test because the time-series collection feature flag is disabled");
        return;
    }

    const testDB = db.getSiblingDB(jsTestName());
    assert.commandWorked(testDB.dropDatabase());

    const timeFieldName = "time";

    const collA = testDB.getCollection("a");
    const collB = testDB.getCollection("b");
    collA.drop();
    collB.drop();

    assert.commandWorked(
        testDB.createCollection(collA.getName(), { timeseries: { timeField: timeFieldName } }));
    assert.commandWorked(
        testDB.createCollection(collB.getName(), { timeseries: { timeField: timeFieldName } }));

    Random.setRandomSeed();

    const numHosts = 10;
    const hosts = TimeseriesTest.generateHosts(numHosts);
    for (let i = 0; i < numHosts; i++) {
        let host = hosts[i];
        assert.commandWorked(collA.insert({
            measurement: "cpu",
            time: ISODate(),
            fields: host.fields,
            tags: host.tags,
        }));
    }
    for (let i = 0; i < 200; i++) {
        let host = TimeseriesTest.getRandomElem(hosts);
        TimeseriesTest.updateUsages(host.fields);
        assert.commandWorked(collB.insert({
            measurement: "cpu",
            time: ISODate(),
            fields: host.fields,
            tags: host.tags,
        }));
    }

    let result = collA.aggregate([
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
    ])
    print("the answer is");
    print(tojson(result.toArray()));
})();
