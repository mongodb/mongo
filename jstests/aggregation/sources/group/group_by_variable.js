/*
 * Test that $group works when $$ROOT is passed for _id. This is intended to reproduce
 * SERVER-37459.
 */
(function() {
"use strict";

const coll = db.group_by_system_var;
coll.drop();

assert.commandWorked(coll.insert({_id: 1, x: 1}));
assert.commandWorked(coll.insert({_id: 2, x: 2}));

function checkPipeline(pipeline, expectedResults) {
    const res = coll.aggregate(pipeline).toArray();
    assert.eq(res, expectedResults, pipeline);
}

const wholeCollUnderId = [{_id: {_id: 1, x: 1}}, {_id: {_id: 2, x: 2}}];
checkPipeline([{$group: {_id: "$$ROOT"}}, {$sort: {"_id": 1}}], wholeCollUnderId);
checkPipeline([{$group: {_id: "$$CURRENT"}}, {$sort: {"_id": 1}}], wholeCollUnderId);

const collIds = [{_id: 1}, {_id: 2}];
checkPipeline([{$group: {_id: "$$ROOT.x"}}, {$sort: {"_id": 1}}], collIds);
checkPipeline([{$group: {_id: "$$CURRENT.x"}}, {$sort: {"_id": 1}}], collIds);
})();
