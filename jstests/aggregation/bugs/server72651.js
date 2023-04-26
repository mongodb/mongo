// SERVER-72651 $match filter is erroneously pushed past $project into COLLSCAN
(function() {

const c = db.server72651;

c.drop();
assert.commandWorked(c.insert({_id: 0, a: 1}));
// The bug caused the query below to return {"_id" : 0} instead of no documents.
assert.eq(
    [],
    c.aggregate([{$project: {"b": 1}}, {$match: {$expr: {$getField: {$literal: "a"}}}}]).toArray());
})();
