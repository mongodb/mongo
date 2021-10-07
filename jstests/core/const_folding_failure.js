// Tests that checks if optimization errors raised with a specific code
// @tags: [
//    requires_fcv_51
// ]

(function() {
"use strict";

const coll = db.getCollection(jsTestName());

coll.drop();

// Forcing optimizer only failure (empty collection, $project will never execute)
assert.throwsWithCode(() => db.nonexistent.aggregate([
    {$project: 
        {dt1: 
            {$map:
                {
                    input: "$a",
                    as: "b",
                    in: {
        $toDate: {
            $dayOfYear: {
                date: new Date("2019-04-08T11:48:29.394Z"),
                timezone: "Australia/Melbourne"
            }
        }
    }}}}}]), 5693200);
})();
