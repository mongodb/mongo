/**
 * Tests that $concatArrays under $bucketAuto preserves incoming document order.
 * @tags: [requires_fcv_81]
 */

const coll = db[jsTestName()];
coll.drop();

const docs = [];
for (let i = 0; i < 10; i++) {
    docs.push({_id: i, n: 9 - i, arr: [i, 42]});
}
coll.insertMany(docs);

{
    // One bucket, all the documents coming out of the $bucketAuto stage should be in the same
    // order as they came in (i.e. sorted by _id).
    const res =
        coll.aggregate([
                {$sort: {_id: 1}},
                {$bucketAuto: {groupBy: "$n", buckets: 1, output: {nums: {$concatArrays: "$arr"}}}}
            ])
            .toArray();
    const expected = [{
        "_id": {"min": 0, "max": 9},
        "nums": [0, 42, 1, 42, 2, 42, 3, 42, 4, 42, 5, 42, 6, 42, 7, 42, 8, 42, 9, 42]
    }];
    assert.eq(
        res,
        expected,
        "$bucketAuto with 1 bucket & $concatArrays does not obey sort order of incoming docs.");
}

{
    // Two buckets, the documents in each bucket should be sorted by _id as well.
    const res =
        coll.aggregate([
                {$sort: {_id: 1}},
                {$bucketAuto: {groupBy: "$n", buckets: 2, output: {nums: {$concatArrays: "$arr"}}}}
            ])
            .toArray();
    const expected = [
        {"_id": {"min": 0, "max": 5}, "nums": [5, 42, 6, 42, 7, 42, 8, 42, 9, 42]},
        {"_id": {"min": 5, "max": 9}, "nums": [0, 42, 1, 42, 2, 42, 3, 42, 4, 42]}
    ];
    assert.eq(
        res,
        expected,
        "$bucketAuto with 2 buckets & $concatArrays does not obey sort order of incoming docs.");
}

{
    // Ensure that if we try to $concatArrays something that evaluates to 'missing', no value gets
    // appended to the resulting array.
    const res =
        coll.aggregate([{
                "$bucketAuto": {
                    "groupBy": "$a",
                    "buckets": 1,
                    "output":
                        {"array": {"$concatArrays": {$getField: {"field": "b", "input": {a: 0}}}}}
                }
            }])
            .toArray();
    const expected = [{"_id": {"min": null, "max": null}, "array": []}];
    assert.eq(res,
              expected,
              "$bucketAuto with $concatArrays does not append any missing values as null.");
}
