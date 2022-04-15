// Tests that the $setVariableFromSubPipeline stage is not allowed in user requests.
(function() {

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert([{_id: "test value"}]));

assert.throwsWithCode(() => db.aggregate([
    {$documents: {}},
    {$setVariableFromSubPipeline: {var : "$$SEARCH_META", pipeline: []}},
    {$replaceWith: "$$SEARCH_META"}
]),
                      5491300);
}());
