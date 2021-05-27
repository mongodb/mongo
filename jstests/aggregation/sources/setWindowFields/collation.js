/**
 * Test that $setWindowFields respects collation for sorting, partitioning, and in the window
 * function.
 */
(function() {
"use strict";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({key: "02", val: "2"}));
assert.commandWorked(coll.insert({key: "2", val: "10"}));
assert.commandWorked(coll.insert({key: "10", val: "3"}));

// Numeric ordering compares numeric strings as numbers, so "10" > "2".
const collation = {
    collation: {locale: "en_US", numericOrdering: true}
};

// Test that sortBy respects collation.
let results =
    coll.aggregate(
            [{
                $setWindowFields: {
                    sortBy: {val: -1},
                    output:
                        {arr: {$push: "$val", window: {documents: ["unbounded", "unbounded"]}}}
                }
            }],
            collation)
        .toArray();
// Test document order before $_internalSetWindowFields rather than $setWindowFields output order.
assert.docEq(results[0].arr, ["10", "3", "2"]);

// Test that partitionBy and window function respect collation.
results =
    coll.aggregate(
            [
                {
                    $setWindowFields: {
                        partitionBy: "$key",
                        output: {
                            minStr: {$min: "$val", window: {documents: ["unbounded", "unbounded"]}},
                            maxStr: {$max: "$val", window: {documents: ["unbounded", "unbounded"]}},
                        }
                    }
                },
                {$project: {_id: 0, val: 0}}
            ],
            collation)
        .toArray();
// Test that "02" and "2" are in the same partition, and the underlying min/max functions also
// respect the collation.
assert.sameMembers(results, [
    {key: "02", minStr: "2", maxStr: "10"},
    {key: "2", minStr: "2", maxStr: "10"},
    {key: "10", minStr: "3", maxStr: "3"},
]);

// Control group: no collation.
results = coll.aggregate([{
                  $setWindowFields: {
                      partitionBy: "$key",
                      output: {
                          minStr: {$min: "$val", window: {documents: ["unbounded", "unbounded"]}},
                          maxStr: {$max: "$val", window: {documents: ["unbounded", "unbounded"]}},
                      }
                  }
              }])
              .toArray();
assert.eq("02", results[0].key);
assert.eq("10", results[1].key);
assert.eq("2", results[2].key);
assert.eq("2", results[0].minStr);
assert.eq("2", results[0].maxStr);
})();
