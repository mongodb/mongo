// Cannot implicitly shard accessed collections because unsupported use of sharded collection
// for target collection of $lookup and $graphLookup.
// @tags: [
//   assumes_unsharded_collection,
// ]

// In SERVER-24714, the 'restrictSearchWithMatch' option was added to $graphLookup. In this file,
// we test the functionality and correctness of the option.

(function() {
"use strict";

let local = db.local;
let foreign = db.foreign;

local.drop();
foreign.drop();

let bulk = foreign.initializeUnorderedBulkOp();
for (let i = 0; i < 100; i++) {
    bulk.insert({_id: i, neighbors: [i - 1, i + 1]});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(local.insert([{starting: 0, foo: 1}, {starting: 1, foo: 2}]));

// Assert that the graphLookup only retrieves ten documents, with _id from 0 to 9.
let res = local
                .aggregate({
                    $graphLookup: {
                        from: "foreign",
                        startWith: "$starting",
                        connectFromField: "neighbors",
                        connectToField: "_id",
                        as: "integers",
                        restrictSearchWithMatch: {_id: {$lt: 10}}
                    }
                })
                .toArray()[0];

assert.eq(res.integers.length, 10);

// Assert that the graphLookup doesn't retrieve any documents, as to do so it would need to
// traverse nodes in the graph that don't match the 'restrictSearchWithMatch' predicate.
res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "neighbors",
                      connectToField: "_id",
                      as: "integers",
                      restrictSearchWithMatch: {_id: {$gt: 10}}
                  }
              })
              .toArray()[0];

assert.eq(res.integers.length, 0);

foreign.drop();
assert.commandWorked(foreign.insert({from: 0, to: 1, shouldBeIncluded: true}));
assert.commandWorked(foreign.insert({from: 1, to: 2, shouldBeIncluded: false}));
assert.commandWorked(foreign.insert({from: 2, to: 3, shouldBeIncluded: true}));

// Assert that the $graphLookup stops exploring when it finds a document that doesn't match the
// filter.
res = local
              .aggregate([{
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "to",
                      connectToField: "from",
                      as: "results",
                      restrictSearchWithMatch: {shouldBeIncluded: true}
                  }
              }, {$match: {starting: 0}}])
              .toArray();

assert.eq(res[0].results.length, 1, tojson(res));

// $expr is allowed inside the 'restrictSearchWithMatch' match expression.
res = local
              .aggregate([{
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "to",
                      connectToField: "from",
                      as: "results",
                      restrictSearchWithMatch: {$expr: {$eq: ["$shouldBeIncluded", true]}}
                  }
              }, {$match: {starting: 0}}])
              .toArray();

assert.eq(res[0].results.length, 1, tojson(res));

// $expr within `restrictSearchWithMatch` has access to variables declared at a higher level.
res = local
                .aggregate([{$sort: {starting: 1}}, {
                    $lookup: {
                        from: "local",
                        let : {foo: true},
                        pipeline: [{
                            $graphLookup: {
                                from: "foreign",
                                startWith: "$starting",
                                connectFromField: "to",
                                connectToField: "from",
                                as: "results",
                                restrictSearchWithMatch:
                                    {$expr: {$eq: ["$shouldBeIncluded", "$$foo"]}}
                            }
                        }, {$sort: {starting: 1}}],
                        as: "array"
                    }
                }, {$match: {starting: 0}}])
                .toArray();

assert.eq(res[0].array[0].results.length, 1, tojson(res));

// $graphLookup which references a let variable defined by $lookup should be treated as correlated.
res = local.aggregate([{
    $lookup: {
        from: "local",
        let : {foo: "$foo"},
        pipeline: [{
            $graphLookup: {
                from: "foreign",
                startWith: "$starting",
                connectFromField: "to",
                connectToField: "from",
                as: "results",
                restrictSearchWithMatch:
                    {$expr: {$eq: ["$from", "$$foo"]}}
            }
        }],
        as: "array"
    }
}, {$sort: {starting: 1}}]).toArray();
assert.eq(2, res.length);
assert.eq(1, res[1].starting, tojson(res));
assert.eq(2, res[1].array.length, tojson(res));
assert.eq(0, res[1].array[0].results.length, tojson(res));
assert.eq(0, res[1].array[1].results.length, tojson(res));
})();
