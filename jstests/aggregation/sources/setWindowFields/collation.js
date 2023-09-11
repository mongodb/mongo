/**
 * Test that $setWindowFields respects collation for sorting, partitioning, and in the window
 * function.
 */
const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({key: "02", valStr: "2", valInt: 20}));
assert.commandWorked(coll.insert({key: "2", valStr: "10", valInt: 5}));
assert.commandWorked(coll.insert({key: "10", valStr: "3", valInt: 12}));

// Numeric ordering compares numeric strings as numbers, so "10" > "2".
const collation = {
    collation: {locale: "en_US", numericOrdering: true}
};

// Test that sortBy respects collation.
let results =
    coll.aggregate(
            [{
                $setWindowFields: {
                    sortBy: {valStr: -1},
                    output:
                        {arr:
                             {$push: "$valStr", window: {documents: ["unbounded", "unbounded"]}}}
                }
            }],
            collation)
        .toArray();
// Test document order before $_internalSetWindowFields rather than $setWindowFields output order.
assert.docEq(["10", "3", "2"], results[0].arr);

// Test that partitionBy respects collation.
results =
    coll.aggregate(
            [
                {
                    $setWindowFields: {
                        partitionBy: "$key",
                        output: {
                            minInt:
                                {$min: "$valInt", window: {documents: ["unbounded", "unbounded"]}},
                            maxInt:
                                {$max: "$valInt", window: {documents: ["unbounded", "unbounded"]}},
                        }
                    }
                },
                {$project: {_id: 0, valInt: 0, valStr: 0}}
            ],
            collation)
        .toArray();
// Test that "02" and "2" are in the same partition
assert.sameMembers(results, [
    {key: "02", minInt: 5, maxInt: 20},
    {key: "2", minInt: 5, maxInt: 20},
    {key: "10", minInt: 12, maxInt: 12},
]);

// Control group: no collation.
results =
    coll.aggregate([
            {
                $setWindowFields: {
                    partitionBy: "$key",
                    output: {
                        minVal: {$min: "$valInt", window: {documents: ["unbounded", "unbounded"]}},
                        maxVal: {$max: "$valInt", window: {documents: ["unbounded", "unbounded"]}},
                    }
                }
            },
            {$project: {_id: 0, valInt: 0, valStr: 0}}
        ])
        .toArray();
// Test that "02" and "2" are in different partitions.
assert.sameMembers(results, [
    {key: "02", minVal: 20, maxVal: 20},
    {key: "2", minVal: 5, maxVal: 5},
    {key: "10", minVal: 12, maxVal: 12},
]);

// Test that rank respects the collation

coll.drop();

assert.commandWorked(coll.insert({key: "02", valStr: "2", valInt: 20}));
assert.commandWorked(coll.insert({key: "2", valStr: "10", valInt: 5}));
assert.commandWorked(coll.insert({key: "3", valStr: "3", valInt: 12}));

results = coll.aggregate(
                  [
                      {
                          $setWindowFields: {
                              sortBy: {key: 1},
                              output: {
                                  rankKeys: {$rank: {}},
                              }
                          }
                      },
                      {$project: {_id: 0, valInt: 0, valStr: 0}}
                  ],
                  collation)
              .toArray();
// Test that "02" and "2" are given the same rank
assert.sameMembers(results, [
    {key: "02", rankKeys: 1},
    {key: "2", rankKeys: 1},
    {key: "3", rankKeys: 3},
]);

// Control group: no collation.
results = coll.aggregate([
                  {
                      $setWindowFields: {
                          sortBy: {key: 1},
                          output: {
                              rankKeys: {$rank: {}},
                          }
                      }
                  },
                  {$project: {_id: 0, valInt: 0, valStr: 0}}
              ])
              .toArray();
assert.sameMembers(results, [
    {key: "02", rankKeys: 1},
    {key: "2", rankKeys: 2},
    {key: "3", rankKeys: 3},
]);

// Test that denseRank respects the collation

results = coll.aggregate(
                  [
                      {
                          $setWindowFields: {
                              sortBy: {key: 1},
                              output: {
                                  rankKeys: {$denseRank: {}},
                              }
                          }
                      },
                      {$project: {_id: 0, valInt: 0, valStr: 0}}
                  ],
                  collation)
              .toArray();
// Test that "02" and "2" are given the same rank
assert.sameMembers(results, [
    {key: "02", rankKeys: 1},
    {key: "2", rankKeys: 1},
    {key: "3", rankKeys: 2},
]);

// Control group: no collation.
results = coll.aggregate([
                  {
                      $setWindowFields: {
                          sortBy: {key: 1},
                          output: {
                              rankKeys: {$denseRank: {}},
                          }
                      }
                  },
                  {$project: {_id: 0, valInt: 0, valStr: 0}}
              ])
              .toArray();
assert.sameMembers(results, [
    {key: "02", rankKeys: 1},
    {key: "2", rankKeys: 2},
    {key: "3", rankKeys: 3},
]);

// Test that min and max respect the collation.

coll.drop();

assert.commandWorked(coll.insert({key: "2", valStr: "2", valInt: 20}));
assert.commandWorked(coll.insert({key: "2", valStr: "10", valInt: 5}));
assert.commandWorked(coll.insert({key: "10", valStr: "3", valInt: 12}));

results =
    coll.aggregate(
            [
                {
                    $setWindowFields: {
                        partitionBy: "$key",
                        output: {
                            minVal:
                                {$min: "$valStr", window: {documents: ["unbounded", "unbounded"]}},
                            maxVal:
                                {$max: "$valStr", window: {documents: ["unbounded", "unbounded"]}},
                        }
                    }
                },
                {$project: {_id: 0, valInt: 0, valStr: 0}}
            ],
            collation)
        .toArray();
assert.sameMembers(results, [
    {key: "2", minVal: "2", maxVal: "10"},
    {key: "2", minVal: "2", maxVal: "10"},
    {key: "10", minVal: "3", maxVal: "3"},
]);

// Control group: no collation.
results =
    coll.aggregate([
            {
                $setWindowFields: {
                    partitionBy: "$key",
                    output: {
                        minVal: {$min: "$valStr", window: {documents: ["unbounded", "unbounded"]}},
                        maxVal: {$max: "$valStr", window: {documents: ["unbounded", "unbounded"]}},
                    }
                }
            },
            {$project: {_id: 0, valInt: 0, valStr: 0}}
        ])
        .toArray();
assert.sameMembers(results, [
    {key: "2", minVal: "10", maxVal: "2"},
    {key: "2", minVal: "10", maxVal: "2"},
    {key: "10", minVal: "3", maxVal: "3"},
]);
