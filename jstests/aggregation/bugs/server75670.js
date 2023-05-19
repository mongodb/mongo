// SERVER-75670 Incorrect projection pushdown caused by dependency analysis for multiple $project
// stages on the same dotted path.
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For resultsEq.

const collName = jsTestName();
const coll = db.getCollection(collName);
coll.drop();

assert.commandWorked(coll.insert([
    {_id: 1, obj: {str: "abc"}},
    {_id: 2, obj: {}},
    {_id: 3, str: "abc"},
]));

assert(resultsEq(
    [
        {_id: 1, obj: {}},
        {_id: 2, obj: {}},
        {_id: 3},
    ],
    coll.aggregate([
            {$project: {"obj.str": 0}},
            {$project: {"obj.str": 1}},
        ])
        .toArray(),
    ));
})();
