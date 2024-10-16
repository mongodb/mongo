/**
 * Tests the $top/$bottom fast paths for blocks of homogeneous data.
 * @tags: [
 *   requires_timeseries
 * ]
 */

import {TimeseriesTest} from "jstests/core/timeseries/libs/timeseries.js";

TimeseriesTest.run((insert) => {
    let coll = db.homogeneous_top_bottom_ts;
    let collNotTs = db.homogeneous_top_bottom_not_ts;

    coll.drop();
    collNotTs.drop();

    // Create a TS collection to get block processing running. Compare this against a classic
    // collection.
    assert.commandWorked(db.createCollection(coll.getName(), {
        timeseries: {timeField: 'time', metaField: 'meta'},
    }));

    assert.commandWorked(db.createCollection(collNotTs.getName()));

    const datePrefix = 1680912440;

    let tsDocs = [];
    let id = 0;
    let dateMax = 0;
    const bucketSize = 25;
    for (let metaIdx = 0; metaIdx < 10; ++metaIdx) {
        let currentDate = 0;
        let metaOffsetSign = metaIdx % 2 == 0 ? -1.0 : 1.0;
        for (let doc = 0; doc < bucketSize; ++doc) {
            let docOffsetSign = doc % 2 == 0 ? -1 : 1;
            tsDocs.push({
                _id: id,
                time: new Date(datePrefix + currentDate +
                               id),  // Use id to ensure uniqueness of each time stamp
                meta: metaIdx,
                x: NumberLong((200 * metaIdx * metaOffsetSign) + (doc * docOffsetSign)),
                y: (1000 * metaIdx * metaOffsetSign) + (doc * docOffsetSign),
                z: NumberLong(id++),
                t: new Date(datePrefix + (10000 * metaIdx * metaOffsetSign) + (doc * docOffsetSign))
            });
            currentDate += 500;
        }
        // Add special doubles to one of the buckets.
        if (metaIdx == 5) {
            let doc = bucketSize;
            for (const dbl of [NaN, -Infinity, +Infinity]) {
                let docOffsetSign = doc % 2 == 0 ? -1 : 1;
                tsDocs.push({
                    _id: id,
                    time: new Date(datePrefix + currentDate +
                                   id),  // Use id to ensure uniqueness of each time stamp
                    meta: metaIdx,
                    x: NumberLong((200 * metaIdx * metaOffsetSign) + (doc * docOffsetSign)),
                    y: dbl,
                    z: NumberInt(id++),
                    t: new Date(datePrefix + (10000 * metaIdx * metaOffsetSign) +
                                (doc * docOffsetSign))
                });
                currentDate += 500;
                doc++;
            }
            dateMax = datePrefix + currentDate;
        }
    }

    assert.commandWorked(collNotTs.insert(tsDocs));
    assert.commandWorked(coll.insert(tsDocs));

    function compareClassicAndBP(pipeline, allowDiskUse) {
        const classicResults = collNotTs.aggregate(pipeline, {allowDiskUse}).toArray();
        const bpResults = coll.aggregate(pipeline, {allowDiskUse}).toArray();

        // Sort order is not guaranteed, so let's sort by the object itself before comparing.
        const cmpFn = function(doc1, doc2) {
            const doc1Json = tojson(doc1);
            const doc2Json = tojson(doc2);
            return doc1Json < doc2Json ? -1 : (doc1Json > doc2Json ? 1 : 0);
        };
        classicResults.sort(cmpFn);
        bpResults.sort(cmpFn);

        function errFn() {
            jsTestLog(collNotTs.explain().aggregate(pipeline, {allowDiskUse}));
            jsTestLog(coll.explain().aggregate(pipeline, {allowDiskUse}));

            return "compareClassicAndBP: Got different results for pipeline " + tojson(pipeline);
        }
        assert.eq(classicResults, bpResults, errFn);
    }

    const nVals = [
        NumberInt(1),
        NumberInt(10),
    ];

    // Since we're only sorting on single fields, every sort field must be unique.
    const sortBys =
        [{time: 1}, {time: -1}, {x: 1}, {x: -1}, {y: 1}, {y: -1}, {z: 1}, {z: -1}, {t: 1}, {t: -1}];

    const groupBys = [
        {time: {"$dateTrunc": {"date": "$time", "unit": "second", binSize: 3}}},
        // Each bucket will contain multiple groups
        {time: {"$dateTrunc": {"date": "$time", "unit": "hour"}}},
        // Every doc will be in the same group.
        {id: null},
        // Every doc will be in the same group.
    ];

    let groupStages = [];
    for (const accumulator of ['$topN', '$bottomN']) {
        for (const nVal of nVals) {
            for (const sortBy of sortBys) {
                for (const groupBy of groupBys) {
                    groupStages.push([
                        {$match: {time: {$gte: new Date(datePrefix), $lte: new Date(dateMax)}}},
                        {
                            $group: {
                                _id: groupBy,
                                acc: {[accumulator]: {n: nVal, sortBy: sortBy, output: "$_id"}}
                            }
                        }
                    ]);
                }
            }
        }
    }

    for (const groupStage of groupStages) {
        compareClassicAndBP(groupStage, true /* allowDiskUse */);
    }
});
