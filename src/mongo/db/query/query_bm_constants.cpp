/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/query/query_bm_constants.h"

#include "mongo/bson/json.h"

namespace mongo {
namespace query_benchmark_constants {
const BSONObj kComplexPredicate = fromjson(R"({
    "$and": [
        {"index": {"$gte": 0, "$lt": 10}},
        {"$or": [
            {
                "index": {"$in": [1, 3, 5, 7, 9]},
                "$expr": {"$eq": [
                    {"$mod": [{"$reduce": {"input": "$array2d", "initialValue": 0, "in": {"$add": ["$$value", {"$sum": "$$this"}]}}}, 2]},
                    1
                ]}
            },
            {
                "index": {"$in": [0, 2, 4, 6, 8]},
                "$expr": {"$eq": [
                    {"$mod": [{"$reduce": {"input": "$array2d", "initialValue": 0, "in": {"$add": ["$$value", {"$sum": "$$this"}]}}}, 2]},
                    0
                ]}
            }
        ]}
    ]
})");

const BSONObj kComplexProjection = fromjson(R"({
    _id: 1,
    index: 1.0,
    array2d: 2,
    arrayHash: {$reduce: {
        input: "$array2d",
        initialValue: 0,
        in: {$mod: [
            {$add: [
                {$multiply: ["$$value", 31]},
                {$reduce: {
                    input: "$$this",
                    initialValue: 0,
                    in: {$mod: [{$add: [{$multiply: ["$$value", 31]}, "$$this"]}, 1000000009]}
                }}
            ]},
            1000000009
        ]}
    }}

})");

const BSONObj kChangeStreamPredicate = fromjson(R"({
  "$and": [
    { "$or": [
        { "$and": [
            { "$or": [
                { "$and": [
                    { "$or": [
                        { "$and": [
                            { "o.to": { "$regex": "^test\\.coll$" } },
                            { "o.renameCollection": { "$exists": true } }
                          ]
                        },
                        { "o.collMod": { "$regex": "^coll$" } },
                        { "o.commitIndexBuild": { "$regex": "^coll$" } },
                        { "o.create": { "$regex": "^coll$" } },
                        { "o.createIndexes": { "$regex": "^coll$" } },
                        { "o.drop": { "$regex": "^coll$" } },
                        { "o.dropIndexes": { "$regex": "^coll$" } },
                        { "o.renameCollection": { "$regex": "^test\\.coll$" } }
                      ]
                    },
                    { "op": { "$eq": "c" } },
                    { "ns": { "$regex": "^test\\.\\$cmd$" } }
                  ]
                },
                { "$and": [
                    { "ns": { "$regex": "^test\\.coll$" } },
                    { "$nor": [
                        { "op": { "$eq": "n" } },
                        { "op": { "$eq": "c" } }
                      ]
                    }
                  ]
                }
              ]
            },
            { "$or": [
                { "$and": [
                    { "op": { "$eq": "u" } },
                    { "o._id": { "$exists": true } }
                  ]
                },
                { "$and": [
                    { "op": { "$eq": "c" } },
                    { "o.drop": { "$exists": true } }
                  ]
                },
                { "$and": [
                    { "op": { "$eq": "c" } },
                    { "o.dropDatabase": { "$exists": true } }
                  ]
                },
                { "$and": [
                    { "op": { "$eq": "c" } },
                    { "o.renameCollection": { "$exists": true } }
                  ]
                },
                { "$and": [
                    { "op": { "$eq": "u" } },
                    { "o._id": { "$not": { "$exists": true } } }
                  ]
                },
                { "op": { "$in": [ "d", "i" ] } }
              ]
            }
          ]
        },
        { "$and": [
            { "$or": [
                { "$and": [
                    { "o.to": { "$eq": "test.coll" } },
                    { "o.renameCollection": { "$exists": true } }
                  ]
                },
                { "o.drop": { "$eq": "coll" } },
                { "o.renameCollection": { "$eq": "test.coll" } }
              ]
            },
            { "ns": { "$eq": "test.$cmd" } },
            { "op": { "$eq": "c" } }
          ]
        },
        { "$and": [
            { "$or": [
                { "o.applyOps": {
                    "$elemMatch": {
                      "$and": [
                        { "$or": [
                            { "o.create": { "$regex": "^coll$" } },
                            { "o.createIndexes": { "$regex": "^coll$" } }
                          ]
                        },
                        { "ns": { "$regex": "^test\\.\\$cmd$" } }
                      ]
                    }
                  }
                },
                { "o.applyOps.ns": { "$regex": "^test\\.coll$" } },
                { "prevOpTime": { "$not": { "$eq": 0 } } }
              ]
            },
            { "op": { "$eq": "c" } },
            { "o.partialTxn": { "$not": { "$eq": true } } },
            { "o.prepare": { "$not": { "$eq": true } } },
            { "o.applyOps": { "$type": [ 4 ] } }
          ]
        },
        { "$and": [
            { "$or": [
                { "o2.refineCollectionShardKey": { "$exists": true } },
                { "o2.reshardBegin": { "$exists": true } },
                { "o2.reshardBlockingWrites": { "$exists": true } },
                { "o2.reshardCollection": { "$exists": true } },
                { "o2.reshardDoneCatchUp": { "$exists": true } },
                { "o2.shardCollection": { "$exists": true } }
              ]
            },
            { "op": { "$eq": "n" } },
            { "ns": { "$regex": "^test\\.coll$" } }
          ]
        },
        { "$and": [
            { "o.commitTransaction": { "$eq": 1 } },
            { "op": { "$eq": "c" } }
          ]
        },
        { "$and": [
            { "op": { "$eq": "n" } },
            { "o2.endOfTransaction": { "$regex": "^test\\.coll$" } }
          ]
        }
      ]
    },
    { "ts": { "$gte": 1729601565 } },
    { "fromMigrate": { "$not": { "$eq": true } } }
  ]
}
)");

const BSONObj kVeryComplexProjection = fromjson(R"({
"reflection": { "$map": {
  "input": [ { "f": "$field", "m": 1 }, { "f": "$zipField", "m": 100 } ],
  "as": "input",
  "in": { "$map": {
    "input": { "$range": [ 1, { "$size": { "$first": "$$input.f" } } ] },
    "as": "y",
    "in": { "$multiply": [
      "$$input.m",
      { "$let": {
        "vars": {
          "smudges": { "$reduce": {
            "input": "$$input.f",
            "initialValue": 0,
            "in": { "$add": [ "$$value",
              { "$let": {
                "vars": { "row": "$$this" },
                "in": { "$reduce": {
                  "input": { "$range": [ 0, "$$y" ] },
                  "initialValue": 0,
                  "in": { "$let": {
                    "vars": {
                      "reflect": { "$arrayElemAt": [ "$$row", { "$add": [ "$$y", { "$subtract": [ "$$y", "$$this" ] }, -1 ] } ] } },
                      "in": { "$cond":
                        { "if": { "$or": [ { "$not": "$$reflect" }, { "$eq": [ "$$reflect", { "$arrayElemAt": [ "$$row", "$$this" ] } ] } ] },
                          "then": "$$value",
                          "else": { "$add": [ "$$value", 1 ] } } } } } } } } } ] } } } },
                          "in": { "$cond": {
                            "if": { "$eq": ["$$smudges", 1] },
                            "then": "$$y",
                            "else": 0
                          } } } } ] } } } } } }
)");
}  // namespace query_benchmark_constants
}  // namespace mongo
