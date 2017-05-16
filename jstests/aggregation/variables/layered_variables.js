// Tests that a pipeline with a blend of variable-using expressions reports correct results.

(function() {
    "use strict";
    const testDB = db.getSiblingDB("layered_variables");
    assert.commandWorked(testDB.dropDatabase());
    const coll = testDB.getCollection("test");

    assert.writeOK(coll.insert({_id: 1, has_permissions: 1, my_array: [2, 3]}));

    const res = assert.commandWorked(testDB.runCommand({
        aggregate: "test",
        pipeline: [
            {
              $addFields: {
                  a: 1,
                  b: {
                      $reduce: {
                          input: "$my_array",
                          initialValue: 1,
                          in : {$multiply: ["$$value", "$$this"]}
                      }
                  },
                  c: {$filter: {input: "$my_array", as: "filter", cond: {$gte: ["$$filter", 0]}}},
                  d: {
                      $let: {
                          vars: {two: 2, three: 3},
                          in : {
                              $multiply: [
                                  "$$two",
                                  "$$three",
                                  {
                                    $let: {
                                        // Variable shadowing here is intentional. It confirms that
                                        // the localy defined variables are used over ones defined
                                        // in the outer scope.
                                        vars: {two: 200, three: 300},
                                        in : {$add: ["$$two", "$$three"]}
                                    }
                                  }

                              ]
                          }
                      }
                  }
              }
            },
            {$redact: {$cond: {if: "$has_permissions", then: "$$DESCEND", else: "$$PRUNE"}}},
            {$addFields: {e: {$map: {input: "$my_array", as: "val", in : {$add: ["$$val", 1]}}}}},
        ],
        cursor: {}
    }));

    assert.eq(
        {_id: 1, has_permissions: 1, my_array: [2, 3], a: 1, b: 6, c: [2, 3], d: 3000, e: [3, 4]},
        res.cursor.firstBatch[0]);
})();
