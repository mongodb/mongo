(function() {
"use strict";

const coll = db.id_meta_projection;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 1, b: 1}));

// Run the aggregate once where the $project can be pushed down.
// @tags: [
//   sbe_incompatible,
// ]
const projectPushedDownRes =
    coll.aggregate([{$sort: {a: 1}}, {$project: {_id: 0, metaField: {$meta: "sortKey"}}}])
        .toArray();

// Run it again where it cannot be pushed down.
const projectNotPushedDownRes = coll.aggregate([
                                        {$sort: {a: 1}},
                                        {$unwind: {path: "$a"}},
                                        {$project: {_id: 0, metaField: {$meta: "sortKey"}}}
                                    ])
                                    .toArray();
assert.eq(projectPushedDownRes, projectNotPushedDownRes);
})();
