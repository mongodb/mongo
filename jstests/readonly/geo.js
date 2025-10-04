import {runReadOnlyTest} from "jstests/readonly/lib/read_only_test.js";

runReadOnlyTest(
    (function () {
        return {
            name: "geo",

            load: function (writableCollection) {
                assert.commandWorked(writableCollection.createIndex({loc: "2dsphere"}));

                let locDocs = [
                    {name: "Berry Park", loc: {type: "Point", coordinates: [40.722396, -73.9573645]}},
                    {
                        name: "Northern Territory",
                        loc: {type: "Point", coordinates: [40.7252334, -73.9595218]},
                    },
                    {
                        name: "Kent Ale House",
                        loc: {type: "Point", coordinates: [40.7223364, -73.9614495]},
                    },
                    {name: "The Shanty", loc: {type: "Point", coordinates: [40.7185752, -73.9510538]}},
                    {
                        name: "The Counting Room",
                        loc: {type: "Point", coordinates: [40.7209601, -73.9588041]},
                    },
                    {name: "Kinfolk 94", loc: {type: "Point", coordinates: [40.7217058, -73.9605489]}},
                ];

                writableCollection.insertMany(locDocs, {ordered: false});
            },
            exec: function (readableCollection) {
                const res = readableCollection
                    .aggregate([
                        {
                            $geoNear: {
                                near: {type: "Point", coordinates: [40.7211404, -73.9591494]},
                                distanceField: "dist",
                                spherical: true,
                            },
                        },
                        {$limit: 1},
                    ])
                    .toArray();
                assert.eq(res[0].name, "The Counting Room", printjson(res));
            },
        };
    })(),
);
