// @tags: [
//   requires_getmore,
// ]

// Test for SERVER-2746

import {IndexUtils} from "jstests/libs/index_utils.js";

let coll = db.geo10;
coll.drop();

assert.commandWorked(coll.createIndex({c: "2d", t: 1}, {min: 0, max: Math.pow(2, 40)}));
IndexUtils.assertIndexes(coll, [{_id: 1}, {c: "2d", t: 1}]);

assert.commandWorked(coll.insert({c: [1, 1], t: 1}));
assert.commandWorked(coll.insert({c: [3600, 3600], t: 1}));
assert.commandWorked(coll.insert({c: [0.001, 0.001], t: 1}));

printjson(
    coll
        .find({
            c: {
                $within: {
                    $box: [
                        [0.001, 0.001],
                        [Math.pow(2, 40) - 0.001, Math.pow(2, 40) - 0.001],
                    ],
                },
            },
            t: 1,
        })
        .toArray(),
);
