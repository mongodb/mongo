// In SERVER-24714, the 'restrictSearchWithMatch' option was added to $graphLookup. In this file,
// we test the functionality and correctness of the option.

(function() {
"use strict";

load("jstests/libs/fixture_helpers.js");  // For isSharded.

var local = db.local;
var foreign = db.foreign;

local.drop();
foreign.drop();

// Do not run the rest of the tests if the foreign collection is implicitly sharded but the flag to
// allow $lookup/$graphLookup into a sharded collection is disabled.
const getShardedLookupParam = db.adminCommand({getParameter: 1, featureFlagShardedLookup: 1});
const isShardedLookupEnabled = getShardedLookupParam.hasOwnProperty("featureFlagShardedLookup") &&
    getShardedLookupParam.featureFlagShardedLookup.value;
if (FixtureHelpers.isSharded(foreign) && !isShardedLookupEnabled) {
    return;
}

var bulk = foreign.initializeUnorderedBulkOp();
for (var i = 0; i < 100; i++) {
    bulk.insert({_id: i, neighbors: [i - 1, i + 1]});
}
assert.commandWorked(bulk.execute());
assert.commandWorked(local.insert({starting: 0}));

// Assert that the graphLookup only retrieves ten documents, with _id from 0 to 9.
var res = local
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
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "to",
                      connectToField: "from",
                      as: "results",
                      restrictSearchWithMatch: {shouldBeIncluded: true}
                  }
              })
              .toArray()[0];

assert.eq(res.results.length, 1);

// $expr is allowed inside the 'restrictSearchWithMatch' match expression.
res = local
              .aggregate({
                  $graphLookup: {
                      from: "foreign",
                      startWith: "$starting",
                      connectFromField: "to",
                      connectToField: "from",
                      as: "results",
                      restrictSearchWithMatch: {$expr: {$eq: ["$shouldBeIncluded", true]}}
                  }
              })
              .toArray()[0];

assert.eq(res.results.length, 1);

// $expr within `restrictSearchWithMatch` has access to variables declared at a higher level.
res = local
                .aggregate([{
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
                        }],
                        as: "array"
                    }
                }])
                .toArray()[0];

assert.eq(res.array[0].results.length, 1);
})();
