// Tests when the granularity rounder needs to approach zero to round correctly. This test was
// designed to reproduce SERVER-57091.
(function() {
"use strict";
const coll = db.server57091;
coll.drop();
coll.insertOne({});

const res =
    coll.aggregate([{
            $bucketAuto: {
                groupBy: {
                    $reduce:
                        {input: [], initialValue: 4.940656484124654e-324, in : {$size: ["$$value"]}}
                },
                buckets: NumberLong("8"),
                granularity: "R80"
            }
        }])
        .toArray();
assert.eq(res, [{_id: {min: 0, max: 1.02e-321}, count: 1}]);
})();
