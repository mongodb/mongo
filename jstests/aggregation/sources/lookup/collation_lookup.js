// Cannot implicitly shard accessed collections because of collection existing when none expected.
// @tags: [assumes_no_implicit_collection_creation_after_drop]

/**
 * Tests that the $lookup stage respects the collation.
 *
 * The comparison of string values between the 'localField' and 'foreignField' should use the
 * collation either explicitly set on the aggregation operation, or the collation inherited from the
 * collection the "aggregate" command was performed on.
 */
(function() {
    "use strict";

    load("jstests/aggregation/extras/utils.js");  // for arrayEq

    const caseInsensitive = {collation: {locale: "en_US", strength: 2}};

    const withDefaultCollationColl = db.collation_lookup_with_default;
    const withoutDefaultCollationColl = db.collation_lookup_without_default;

    withDefaultCollationColl.drop();
    withoutDefaultCollationColl.drop();

    assert.commandWorked(db.createCollection(withDefaultCollationColl.getName(), caseInsensitive));
    assert.writeOK(withDefaultCollationColl.insert({_id: "lowercase", str: "abc"}));

    assert.writeOK(withoutDefaultCollationColl.insert({_id: "lowercase", str: "abc"}));
    assert.writeOK(withoutDefaultCollationColl.insert({_id: "uppercase", str: "ABC"}));
    assert.writeOK(withoutDefaultCollationColl.insert({_id: "unmatched", str: "def"}));

    // Test that the $lookup stage respects the inherited collation.
    let res = withDefaultCollationColl
                  .aggregate([{
                      $lookup: {
                          from: withoutDefaultCollationColl.getName(),
                          localField: "str",
                          foreignField: "str",
                          as: "matched",
                      },
                  }])
                  .toArray();
    assert.eq(1, res.length, tojson(res));

    let expected = [{_id: "lowercase", str: "abc"}, {_id: "uppercase", str: "ABC"}];
    assert(
        arrayEq(expected, res[0].matched),
        "Expected " + tojson(expected) + " to equal " + tojson(res[0].matched) + " up to ordering");

    res = withDefaultCollationColl
              .aggregate([{
                  $lookup: {
                      from: withoutDefaultCollationColl.getName(),
                      let : {str1: "$str"},
                      pipeline: [
                          {$match: {$expr: {$eq: ["$str", "$$str1"]}}},
                          {
                            $lookup: {
                                from: withoutDefaultCollationColl.getName(),
                                let : {str2: "$str"},
                                pipeline: [{$match: {$expr: {$eq: ["$str", "$$str1"]}}}],
                                as: "matched2"
                            }
                          }
                      ],
                      as: "matched1",
                  },
              }])
              .toArray();
    assert.eq(1, res.length, tojson(res));

    expected = [
        {
          "_id": "lowercase",
          "str": "abc",
          "matched2": [{"_id": "lowercase", "str": "abc"}, {"_id": "uppercase", "str": "ABC"}]
        },
        {
          "_id": "uppercase",
          "str": "ABC",
          "matched2": [{"_id": "lowercase", "str": "abc"}, {"_id": "uppercase", "str": "ABC"}]
        }
    ];
    assert(arrayEq(expected, res[0].matched1),
           "Expected " + tojson(expected) + " to equal " + tojson(res[0].matched1) +
               " up to ordering. " + tojson(res));

    // Test that the $lookup stage respects the inherited collation when it optimizes with an
    // $unwind stage.
    res = withDefaultCollationColl
              .aggregate([
                  {
                    $lookup: {
                        from: withoutDefaultCollationColl.getName(),
                        localField: "str",
                        foreignField: "str",
                        as: "matched",
                    },
                  },
                  {$unwind: "$matched"},
              ])
              .toArray();
    assert.eq(2, res.length, tojson(res));

    expected = [
        {_id: "lowercase", str: "abc", matched: {_id: "lowercase", str: "abc"}},
        {_id: "lowercase", str: "abc", matched: {_id: "uppercase", str: "ABC"}}
    ];
    assert(arrayEq(expected, res),
           "Expected " + tojson(expected) + " to equal " + tojson(res) + " up to ordering");

    res = withDefaultCollationColl
              .aggregate([
                  {
                    $lookup: {
                        from: withoutDefaultCollationColl.getName(),
                        let : {str1: "$str"},
                        pipeline: [
                            {$match: {$expr: {$eq: ["$str", "$$str1"]}}},
                            {
                              $lookup: {
                                  from: withoutDefaultCollationColl.getName(),
                                  let : {str2: "$str"},
                                  pipeline: [{$match: {$expr: {$eq: ["$str", "$$str1"]}}}],
                                  as: "matched2"
                              }
                            },
                            {$unwind: "$matched2"},
                        ],
                        as: "matched1",
                    },
                  },
                  {$unwind: "$matched1"},
              ])
              .toArray();
    assert.eq(4, res.length, tojson(res));

    expected = [
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "lowercase", "str": "abc", "matched2": {"_id": "lowercase", "str": "abc"}}
        },
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "lowercase", "str": "abc", "matched2": {"_id": "uppercase", "str": "ABC"}}
        },
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "uppercase", "str": "ABC", "matched2": {"_id": "lowercase", "str": "abc"}}
        },
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "uppercase", "str": "ABC", "matched2": {"_id": "uppercase", "str": "ABC"}}
        }
    ];
    assert(arrayEq(expected, res),
           "Expected " + tojson(expected) + " to equal " + tojson(res) + " up to ordering");

    // Test that the $lookup stage respects an explicit collation on the aggregation operation.
    res = withoutDefaultCollationColl
              .aggregate(
                  [
                    {$match: {_id: "lowercase"}},
                    {
                      $lookup: {
                          from: withoutDefaultCollationColl.getName(),
                          localField: "str",
                          foreignField: "str",
                          as: "matched",
                      },
                    },
                  ],
                  caseInsensitive)
              .toArray();
    assert.eq(1, res.length, tojson(res));

    expected = [{_id: "lowercase", str: "abc"}, {_id: "uppercase", str: "ABC"}];
    assert(
        arrayEq(expected, res[0].matched),
        "Expected " + tojson(expected) + " to equal " + tojson(res[0].matched) + " up to ordering");

    res = withoutDefaultCollationColl
              .aggregate(
                  [
                    {$match: {_id: "lowercase"}},
                    {
                      $lookup: {
                          from: withoutDefaultCollationColl.getName(),
                          let : {str1: "$str"},
                          pipeline: [
                              {$match: {$expr: {$eq: ["$str", "$$str1"]}}},
                              {
                                $lookup: {
                                    from: withoutDefaultCollationColl.getName(),
                                    let : {str2: "$str"},
                                    pipeline: [{$match: {$expr: {$eq: ["$str", "$$str1"]}}}],
                                    as: "matched2"
                                }
                              }
                          ],
                          as: "matched1",
                      },
                    }
                  ],
                  caseInsensitive)
              .toArray();
    assert.eq(1, res.length, tojson(res));

    expected = [
        {
          "_id": "lowercase",
          "str": "abc",
          "matched2": [{"_id": "lowercase", "str": "abc"}, {"_id": "uppercase", "str": "ABC"}]
        },
        {
          "_id": "uppercase",
          "str": "ABC",
          "matched2": [{"_id": "lowercase", "str": "abc"}, {"_id": "uppercase", "str": "ABC"}]
        }
    ];
    assert(arrayEq(expected, res[0].matched1),
           "Expected " + tojson(expected) + " to equal " + tojson(res[0].matched1) +
               " up to ordering");

    // Test that the $lookup stage respects an explicit collation on the aggregation operation when
    // it optimizes with an $unwind stage.
    res = withoutDefaultCollationColl
              .aggregate(
                  [
                    {$match: {_id: "lowercase"}},
                    {
                      $lookup: {
                          from: withoutDefaultCollationColl.getName(),
                          localField: "str",
                          foreignField: "str",
                          as: "matched",
                      },
                    },
                    {$unwind: "$matched"},
                  ],
                  caseInsensitive)
              .toArray();
    assert.eq(2, res.length, tojson(res));

    expected = [
        {_id: "lowercase", str: "abc", matched: {_id: "lowercase", str: "abc"}},
        {_id: "lowercase", str: "abc", matched: {_id: "uppercase", str: "ABC"}}
    ];
    assert(arrayEq(expected, res),
           "Expected " + tojson(expected) + " to equal " + tojson(res) + " up to ordering");

    res = withoutDefaultCollationColl
              .aggregate(
                  [
                    {$match: {_id: "lowercase"}},
                    {
                      $lookup: {
                          from: withoutDefaultCollationColl.getName(),
                          let : {str1: "$str"},
                          pipeline: [
                              {$match: {$expr: {$eq: ["$str", "$$str1"]}}},
                              {
                                $lookup: {
                                    from: withoutDefaultCollationColl.getName(),
                                    let : {str2: "$str"},
                                    pipeline: [{$match: {$expr: {$eq: ["$str", "$$str1"]}}}],
                                    as: "matched2"
                                }
                              },
                              {$unwind: "$matched2"},
                          ],
                          as: "matched1",
                      },
                    },
                    {$unwind: "$matched1"},
                  ],
                  caseInsensitive)
              .toArray();
    assert.eq(4, res.length, tojson(res));

    expected = [
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "lowercase", "str": "abc", "matched2": {"_id": "lowercase", "str": "abc"}}
        },
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "lowercase", "str": "abc", "matched2": {"_id": "uppercase", "str": "ABC"}}
        },
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "uppercase", "str": "ABC", "matched2": {"_id": "lowercase", "str": "abc"}}
        },
        {
          "_id": "lowercase",
          "str": "abc",
          "matched1":
              {"_id": "uppercase", "str": "ABC", "matched2": {"_id": "uppercase", "str": "ABC"}}
        }
    ];
    assert(arrayEq(expected, res),
           "Expected " + tojson(expected) + " to equal " + tojson(res) + " up to ordering");

    // Test that the $lookup stage uses the "simple" collation if a collation isn't set on the
    // collection or the aggregation operation.
    res = withoutDefaultCollationColl
              .aggregate([
                  {$match: {_id: "lowercase"}},
                  {
                    $lookup: {
                        from: withDefaultCollationColl.getName(),
                        localField: "str",
                        foreignField: "str",
                        as: "matched",
                    },
                  },
              ])
              .toArray();
    assert.eq([{_id: "lowercase", str: "abc", matched: [{_id: "lowercase", str: "abc"}]}], res);

    res = withoutDefaultCollationColl
              .aggregate([
                  {$match: {_id: "lowercase"}},
                  {
                    $lookup: {
                        from: withoutDefaultCollationColl.getName(),
                        let : {str1: "$str"},
                        pipeline: [
                            {$match: {$expr: {$eq: ["$str", "$$str1"]}}},
                            {
                              $lookup: {
                                  from: withoutDefaultCollationColl.getName(),
                                  let : {str2: "$str"},
                                  pipeline: [{$match: {$expr: {$eq: ["$str", "$$str1"]}}}],
                                  as: "matched2"
                              }
                            },
                            {$unwind: "$matched2"},
                        ],
                        as: "matched1",
                    },
                  },
              ])
              .toArray();
    assert.eq([{
                 "_id": "lowercase",
                 "str": "abc",
                 "matched1": [{
                     "_id": "lowercase",
                     "str": "abc",
                     "matched2": {"_id": "lowercase", "str": "abc"}
                 }]
              }],
              res);
})();
