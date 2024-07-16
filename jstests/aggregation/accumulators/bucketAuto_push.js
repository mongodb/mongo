/**
 * Tests that $push under $bucketAuto preserves incoming document order.
 */

const coll = db[jsTestName()];
coll.drop();

const docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({_id: i, n: 9 - i});
}
coll.insertMany(docs);

{
    // One bucket, all the documents coming out of the $bucketAuto stage should be in the same
    // order as they came in (i.e. sorted by _id).
    const res =
        coll.aggregate([
                {$sort: {_id: 1}},
                {$bucketAuto: {groupBy: "$n", buckets: 1, output: {docs: {$push: "$$ROOT"}}}}
            ])
            .toArray();
    const expected = [{
        "_id": {"min": 0, "max": 9},
        "docs": [
            {"_id": 0, "n": 9},
            {"_id": 1, "n": 8},
            {"_id": 2, "n": 7},
            {"_id": 3, "n": 6},
            {"_id": 4, "n": 5},
            {"_id": 5, "n": 4},
            {"_id": 6, "n": 3},
            {"_id": 7, "n": 2},
            {"_id": 8, "n": 1},
            {"_id": 9, "n": 0}
        ]
    }];
    assert.eq(res,
              expected,
              "$bucketAuto with 1 bucket & $push does not obey sort order of incoming docs.");
}

{
    // Two buckets, the documents in each bucket should be sorted by _id as well.
    const res =
        coll.aggregate([
                {$sort: {_id: 1}},
                {$bucketAuto: {groupBy: "$n", buckets: 2, output: {docs: {$push: "$$ROOT"}}}}
            ])
            .toArray();
    const expected = [
        {
            "_id": {"min": 0, "max": 5},
            "docs": [
                {"_id": 5, "n": 4},
                {"_id": 6, "n": 3},
                {"_id": 7, "n": 2},
                {"_id": 8, "n": 1},
                {"_id": 9, "n": 0}
            ]
        },
        {
            "_id": {"min": 5, "max": 9},
            "docs": [
                {"_id": 0, "n": 9},
                {"_id": 1, "n": 8},
                {"_id": 2, "n": 7},
                {"_id": 3, "n": 6},
                {"_id": 4, "n": 5}
            ]
        }
    ];
    assert.eq(res,
              expected,
              "$bucketAuto with 2 buckets & $push does not obey sort order of incoming docs.");
}

{
    // Ensure that if we try to $push something that evaluates to 'missing', no value gets pushed to
    // the resulting array.
    const res =
        coll.aggregate([{
                "$bucketAuto": {
                    "groupBy": "$a",
                    "buckets": 1,
                    "output": {"array": {"$push": {$getField: {"field": "b", "input": {a: 0}}}}}
                }
            }])
            .toArray();
    const expected = [{"_id": {"min": null, "max": null}, "array": []}];
    assert.eq(res, expected, "$bucketAuto with $push does not push any missing values as null.");
}
