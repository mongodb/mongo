/**
 * Tests the behavior of a $lookup in a sharded environment. These include tests where the 'from'
 * collection is sharded when the local collection is sharded and unsharded, when the $lookup
 * subpipeline can target certain shards or is scatter-gather, and when the $lookup subpipeline
 * contains a nested $lookup stage. This also includes tests when the mongos has stale information
 * about the foreign collection.
 *
 * Shard targeting logic for $lookup changed in 7.3 and may not match the expected behavior in a
 * multiversion environment.
 * @tags: [
 *   requires_fcv_73,
 * ]
 */

import {resultsEq} from "jstests/aggregation/extras/utils.js";
import {enableLocalReadLogs, getLocalReadCount} from "jstests/libs/local_reads.js";
import {
    profilerHasNumMatchingEntriesOrThrow,
    profilerHasZeroMatchingEntriesOrThrow
} from "jstests/libs/profiler.js";

const st = new ShardingTest({shards: 2, mongos: 2});
const testName = "sharded_lookup";

const mongosDB = st.s0.getDB(testName);
const shardList = [st.shard0.getDB(testName), st.shard1.getDB(testName)];

assert.commandWorked(
    mongosDB.adminCommand({enableSharding: mongosDB.getName(), primaryShard: st.shard0.shardName}));

// Turn on the profiler and increase the query log level for both shards.
assert.commandWorked(st.shard0.getDB(testName).setProfilingLevel(2));
assert.commandWorked(st.shard1.getDB(testName).setProfilingLevel(2));
enableLocalReadLogs(st.shard0);
enableLocalReadLogs(st.shard1);

const ordersColl = mongosDB.orders;
const reviewsColl = mongosDB.reviews;
const updatesColl = mongosDB.updates;

// 'freshMongos' and 'freshReviews' are used later to shard/drop the 'reviews' collection. Note that
// we insert into the reviews collection only through 'freshReviews' so that 'freshMongos' has up to
// date information about its sharding state, but 'mongosDB' may have stale information.
const freshMongos = st.s1;
const freshReviews = freshMongos.getDB(testName)[reviewsColl.getName()];

function getNode(i) {
    return i === 0 ? st.shard0 : st.shard1;
}

function assertLocalReadCount(comment, actualCount, expectedCount) {
    assert.eq(actualCount,
              expectedCount,
              "Expected to find " + expectedCount + " local reads but found " + actualCount +
                  " instead for " + comment + ".");
}

function assertLookupExecution(pipeline, opts, expected) {
    assert.commandWorked(ordersColl.insert([
        {_id: 0, customer: "Alice", products: [{_id: "hat", price: 20}, {_id: "shirt", price: 30}]},
        {
            _id: 1,
            customer: "Barbara",
            products: [{_id: "shirt", price: 30}, {_id: "bowl", price: 6}]
        }
    ]));
    // This insert lets 'freshMongos' know about the latest state of 'freshReviews', but 'mongosDB'
    // may have a stale view.
    assert.commandWorked(freshReviews.insert([
        {_id: 0, product_id: "hat", stars: 4.5, comment: "super!"},
        {_id: 1, product_id: "hat", stars: 4, comment: "good"},
        {_id: 2, product_id: "shirt", stars: 3, comment: "meh"},
        {_id: 3, product_id: "bowl", stars: 4, comment: "it's a bowl"},
    ]));
    assert.commandWorked(updatesColl.insert([
        {_id: 0, original_review_id: 0, product_id: "hat", updated_stars: 4},
        {_id: 1, original_review_id: 1, product_id: "hat", updated_stars: 1},
        {_id: 2, original_review_id: 3, product_id: "bowl", updated_stars: 5},
    ]));

    // Here we perform the query through 'mongosDB', which may still have a stale view of the
    // 'reviews' collection.
    let actual = ordersColl.aggregate(pipeline, opts).toArray();
    assert(resultsEq(expected.results, actual),
           "Expected to see results: " + tojson(expected.results) + " but got: " + tojson(actual));

    // If the primary delegates the merging functionality to a randomly chosen shard, confirm the
    // expected behavior here.
    if (expected.randomlyDelegatedMerger) {
        let totalLookupExecution = 0;
        for (const shard of shardList) {
            totalLookupExecution += shard.system.profile
                                        .find({
                                            "command.aggregate": ordersColl.getName(),
                                            "command.comment": opts.comment,
                                            "command.pipeline.$mergeCursors": {$exists: true},
                                            "command.pipeline.$lookup": {$exists: true}
                                        })
                                        .itcount();
        }
        assert.eq(totalLookupExecution, 1);
    }

    for (let i = 0; i < shardList.length; i++) {
        // Confirm that the $lookup execution is as expected.
        if (!expected.randomlyDelegatedMerger) {
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: shardList[i],
                filter: {
                    "command.aggregate": ordersColl.getName(),
                    "command.comment": opts.comment,
                    "command.pipeline.$lookup": {$exists: true}
                },
                numExpectedMatches: expected.toplevelExec[i]
            });

            // Confirm that the $lookup subpipeline execution is as expected.
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: shardList[i],
                filter: {
                    "command.aggregate": reviewsColl.getName(),
                    "command.comment": opts.comment,
                    "command.fromMongos": expected.mongosMerger === true,
                    "errName": {$ne: "StaleConfig"}
                },
                numExpectedMatches: expected.subpipelineExec[i]
            });
            if (expected.subpipelineLocalExec) {
                assertLocalReadCount(
                    opts.comment,
                    getLocalReadCount(getNode(i), reviewsColl.getFullName(), opts.comment),
                    expected.subpipelineLocalExec[i]);
            }
        } else {
            // If merger is randomly delegated, we can't know in advance if reads will be local or
            // remote.
            const localReadCount =
                getLocalReadCount(getNode(i), reviewsColl.getFullName(), opts.comment);
            if (localReadCount === 0) {
                profilerHasNumMatchingEntriesOrThrow({
                    profileDB: shardList[i],
                    filter: {
                        "command.aggregate": reviewsColl.getName(),
                        "command.comment": opts.comment,
                        "command.fromMongos": expected.mongosMerger === true,
                        "errName": {$ne: "StaleConfig"}
                    },
                    numExpectedMatches: expected.subpipelineExec[i]
                });
            } else {
                assertLocalReadCount(opts.comment, localReadCount, expected.subpipelineExec[i]);
            }
        }

        // If there is a nested $lookup within the top-level $lookup subpipeline, confirm that
        // execution is as expected.
        if (expected.nestedExec) {
            // Confirm that a nested $lookup is never on the shards part of the pipeline split and
            // doesn't get dispatched to a foreign shard (that is, we should designate a merging
            // shard to execute the nested $lookup).
            profilerHasZeroMatchingEntriesOrThrow({
                profileDB: shardList[i],
                filter: {
                    "command.aggregate": reviewsColl.getName(),
                    "command.comment": opts.comment,
                    "errName": {$ne: "StaleConfig"},
                    "command.pipeline.$lookup": {$exists: true}
                }
            });

            // Confirm that the nested $lookup subpipeline execution is as expected.
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: shardList[i],
                filter: {
                    "command.aggregate": updatesColl.getName(),
                    "command.comment": opts.comment,
                    "errName": {$ne: "StaleConfig"}
                },
                numExpectedMatches: expected.nestedExec[i]
            });
        }

        if (expected.nestedLocalExec) {
            assertLocalReadCount(
                opts.comment,
                getLocalReadCount(getNode(i), updatesColl.getFullName(), opts.comment),
                expected.nestedLocalExec[i]);
        }

        // If there is an additional top-level $lookup, confirm that execution is as expected.
        if (expected.multipleLookups) {
            // Confirm that the second $lookup execution is as expected.
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: shardList[i],
                filter: {
                    "command.aggregate": ordersColl.getName(),
                    "command.comment": opts.comment,
                    "command.pipeline.$lookup.from": {$eq: updatesColl.getName()}
                },
                numExpectedMatches: expected.multipleLookups.toplevelExec[i]
            });

            // Confirm that the second $lookup subpipeline execution is as expected.
            profilerHasNumMatchingEntriesOrThrow({
                profileDB: shardList[i],
                filter:
                    {"command.aggregate": updatesColl.getName(), "command.comment": opts.comment},
                numExpectedMatches: expected.multipleLookups.subpipelineExec[i]
            });

            assertLocalReadCount(
                opts.comment,
                getLocalReadCount(getNode(i), updatesColl.getFullName(), opts.comment),
                expected.multipleLookups.subpipelineLocalExec[i]);
        }
    }
    assert(ordersColl.drop());
    assert(reviewsColl.drop());
    assert(updatesColl.drop());
}

// Test unsharded local collection and sharded foreign collection, with a targeted $lookup.
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());

let pipeline = [
    {$match: {customer: "Alice"}},
    {$unwind: "$products"},
    {
        $lookup:
            {from: "reviews", localField: "products._id", foreignField: "product_id", as: "reviews"}
    },
    {
        $group: {
            _id: "$_id",
            products: {
                $push: {
                    _id: "$products._id",
                    price: "$products.price",
                    avg_review: {$avg: "$reviews.stars"}
                }
            }
        }
    }
];

let expectedRes = [{
    _id: 0,
    products: [{_id: "hat", price: 20, avg_review: 4.25}, {_id: "shirt", price: 30, avg_review: 3}]
}];

assertLookupExecution(pipeline, {comment: "unsharded_to_sharded_targeted"}, {
    results: expectedRes,
    // Because the local collection is unsharded, the $lookup stage is executed on the primary
    // shard of the database.
    toplevelExec: [1, 0],
    // For every document that flows through the $lookup stage, the node executing the $lookup will
    // target the shard that holds the relevant data for the sharded foreign collection.
    subpipelineExec: [0, 2]
});

// Test unsharded local collection and sharded foreign collection, with an untargeted $lookup.
st.shardColl(reviewsColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

assertLookupExecution(pipeline, {comment: "unsharded_to_sharded_scatter"}, {
    results: expectedRes,
    // Because the local collection is unsharded, the $lookup stage is executed on the primary
    // shard of the database.
    toplevelExec: [1, 0],
    // For every document that flows through the $lookup stage, the node executing the $lookup will
    // perform a scatter-gather query and open a cursor on every shard that contains the foreign
    // collection.
    subpipelineExec: [2, 2]
});

// Test sharded local collection and sharded foreign collection, with a targeted $lookup.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());

pipeline = [
    {$unwind: "$products"},
    {
        $lookup:
            {from: "reviews", localField: "products._id", foreignField: "product_id", as: "reviews"}
    },
    {
        $group: {
            _id: "$_id",
            products: {
                $push: {
                    _id: "$products._id",
                    price: "$products.price",
                    avg_review: {$avg: "$reviews.stars"}
                }
            }
        }
    }
];

expectedRes = [
    {
        _id: 0,
        products:
            [{_id: "hat", price: 20, avg_review: 4.25}, {_id: "shirt", price: 30, avg_review: 3}]
    },
    {
        _id: 1,
        products: [{_id: "shirt", price: 30, avg_review: 3}, {_id: "bowl", price: 6, avg_review: 4}]
    }
];
assertLookupExecution(pipeline, {comment: "sharded_to_sharded_targeted"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the $lookup stage is executed in parallel on every
    // shard that contains the local collection.
    toplevelExec: [1, 1],
    // Each node executing the $lookup will, for every document that flows through the $lookup
    // stage, target the shard(s) that holds the relevant data for the sharded foreign collection.
    subpipelineExec: [1, 2],
    // In cases, when shard targets itself, it will not show up in profiler, as the read will be
    // performed without remote cursor.
    subpipelineLocalExec: [0, 1]
});

// Test sharded local collection and sharded foreign collection, with an untargeted $lookup.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(reviewsColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

assertLookupExecution(pipeline, {comment: "sharded_to_sharded_scatter"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the $lookup stage is executed in parallel on every
    // shard that contains the local collection.
    toplevelExec: [1, 1],
    // Each node executing the $lookup will, for every document that flows through the $lookup
    // stage, perform a scatter-gather query and open a cursor on every shard that contains the
    // foreign collection.
    subpipelineExec: [4, 4]
});

// Test sharded local collection and sharded foreign collection with a targeted top-level $lookup
// and a nested $lookup on an unsharded foreign collection.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());

pipeline = [
    {$match: {customer: "Alice"}},
    {$unwind: "$products"},
    {$lookup: {
        from: "reviews",
        let: {customers_product_id: "$products._id"},
        pipeline: [
            {$match: {$expr: {$eq: ["$product_id", "$$customers_product_id"]}}},
            {$lookup: {
                from: "updates",
                let: {review_id: "$_id"},
                pipeline: [{$match: {$expr: {$eq: ["$original_review_id", "$$review_id"]}}}],
                as: "updates"
            }},
            {$unwind: {path: "$updates", preserveNullAndEmptyArrays: true}},
            {$project: {product_id: 1, stars: {$ifNull: ["$updates.updated_stars", "$stars"]}}}
        ],
        as: "reviews"
    }},
    {$group: {
        _id: "$_id",
        products: {$push: {
            _id: "$products._id",
            price: "$products.price",
            avg_review: {$avg: "$reviews.stars"}
        }}
    }}
];

expectedRes = [{
    _id: 0,
    products: [{_id: "hat", price: 20, avg_review: 2.5}, {_id: "shirt", price: 30, avg_review: 3}]
}];

assertLookupExecution(pipeline, {comment: "sharded_to_sharded_to_unsharded"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the $lookup stage is executed in parallel on every
    // shard that contains the local collection.
    toplevelExec: [1, 1],
    // Each node executing the $lookup will, for every document that flows through the $lookup
    // stage, target the shard that holds the relevant data for the sharded foreign collection.
    subpipelineExec: [0, 2],
    // When executing the subpipeline, the nested $lookup stage will stay on the merging half of the
    // pipeline and execute on the merging node, sending requests to execute the nested subpipelines
    // on the primary shard (where the unsharded 'updates' collection is stored). The $lookup on the
    // non-primary shard would need to send requests over the network (not needed in this case due
    // to the initial $match on customer); the rest are done via local reads and are not logged.
    nestedExec: [0, 0]
});

// Test sharded local collection and sharded foreign collection with a targeted top-level $lookup
// and a nested targeted $lookup on a sharded foreign collection.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());
st.shardColl(updatesColl,
             {original_review_id: 1},
             {original_review_id: 1},
             {original_review_id: 1},
             mongosDB.getName());

assertLookupExecution(pipeline, {comment: "sharded_to_sharded_to_sharded_targeted"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the top-level stage $lookup is executed in parallel on
    // every shard that contains the local collection.
    toplevelExec: [1, 1],
    // For every document that flows through the $lookup stage, the node executing the $lookup will
    // target the shard(s) that holds the relevant data for the sharded foreign collection.
    subpipelineExec: [0, 2],
    // When executing the subpipeline, the nested $lookup stage will stay on the merging half of the
    // pipeline and execute on the merging node, targeting shards to execute the nested $lookup
    // subpipeline.
    nestedExec: [0, 2],
    nestedLocalExec: [1, 0],
});

// Test sharded local collection and sharded foreign collection with a targeted top-level $lookup
// and a nested untargeted $lookup on a sharded foreign collection.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());
st.shardColl(updatesColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());

assertLookupExecution(pipeline, {comment: "sharded_to_sharded_to_sharded_scatter"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the $lookup stage is executed in parallel on every
    // shard that contains the local collection.
    toplevelExec: [1, 1],
    // For every document that flows through the $lookup stage, the node executing the $lookup will
    // target the shard that holds the relevant data for the sharded foreign collection.
    subpipelineExec: [0, 2],
    // When executing the subpipeline, the nested $lookup stage will stay on the merging half of the
    // pipeline and execute on the merging node, performing a scatter-gather query to execute the
    // nested $lookup subpipeline.
    nestedExec: [3, 3]
});

// Test sharded local collection where the foreign namespace is a sharded view with another
// $lookup against a sharded collection. Note that the $lookup in the view should be treated as
// "nested" $lookup and should execute on the merging node.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());
st.shardColl(updatesColl,
             {original_review_id: 1},
             {original_review_id: 1},
             {original_review_id: 1},
             mongosDB.getName());

assert.commandWorked(mongosDB.createView("reviewsView", reviewsColl.getName(),
    [{$lookup: {
        from: "updates",
        let: {review_id: "$_id"},
        pipeline: [{$match: {$expr: {$eq: ["$original_review_id", "$$review_id"]}}}],
        as: "updates"
    }},
    {$unwind: {path: "$updates", preserveNullAndEmptyArrays: true}},
    {$project: {product_id: 1, stars: {$ifNull: ["$updates.updated_stars", "$stars"]}}}
]));
pipeline = [
    {$match: {customer: "Alice"}},
    {$unwind: "$products"},
    {$lookup: {
        from: "reviewsView",
        let: {customers_product_id: "$products._id"},
        pipeline: [
            {$match: {$expr: {$eq: ["$product_id", "$$customers_product_id"]}}},
        ],
        as: "reviews"
    }},
    {$group: {
        _id: "$_id",
        products: {$push: {
            _id: "$products._id",
            price: "$products.price",
            avg_review: {$avg: "$reviews.stars"}
        }}
    }}
];

assertLookupExecution(pipeline, {comment: "sharded_to_sharded_view_to_sharded"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, but mongos does not know that the foreign namespace is
    // a view on a sharded collection. It is instead treated as an unsharded collection, and the
    // top-level $lookup is only on the primary.
    toplevelExec: [1, 0],
    // For every document that flows through the $lookup stage, the primary will target the shard(s)
    // that holds the relevant data for the sharded foreign view.
    subpipelineExec: [0, 2],
    // When executing the subpipeline, the "nested" $lookup stage contained in the view pipeline
    // will stay on the merging half of the pipeline and execute on the merging node, targeting
    // shards to execute the nested subpipeline.
    nestedExec: [0, 2],
    nestedLocalExec: [1, 0],
});

// Test that a targeted $lookup on a sharded collection can execute correctly on mongos.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());

pipeline = [
    {$match: {customer: "Alice"}},
    {$unwind: "$products"},
    {$group: {_id: "$_id", products: {$push: {_id: "$products._id"}}}},
    {$unwind: "$products"},
    {$project: {_id: "$products._id"}},
    {$lookup: {
        from: "reviews",
        let: {customers_product_id: "$_id"},
        pipeline: [
           {$match: {$expr: {$eq: ["$product_id", "$$customers_product_id"]}}},
           {$project: {comment: 1, _id: 0}}
        ],
        as: "reviews"
     }},
];

expectedRes = [
    {_id: "hat", reviews: [{comment: "super!"}, {comment: "good"}]},
    {_id: "shirt", reviews: [{comment: "meh"}]}
];

assertLookupExecution(
    pipeline, {comment: "sharded_to_sharded_on_mongos_targeted", allowDiskUse: false}, {
        results: expectedRes,
        // Because the $lookup is after a $group that requires merging, the $lookup stage is
        // executed on mongos.
        toplevelExec: [0, 0],
        mongosMerger: true,
        // For every document that flows through the $lookup stage, the mongos executing the $lookup
        // will target the shard that holds the relevant data for the sharded foreign collection.
        subpipelineExec: [0, 2]
    });

// Test that an untargeted $lookup on a sharded collection can execute correctly on mongos.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(reviewsColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

assertLookupExecution(
    pipeline, {comment: "sharded_to_sharded_on_mongos_untargeted", allowDiskUse: false}, {
        results: expectedRes,
        // Because the $lookup is after a $group that requires merging, the $lookup stage is
        // executed on mongos.
        toplevelExec: [0, 0],
        mongosMerger: true,
        // For every document that flows through the $lookup stage, the mongos executing the $lookup
        // will perform a scatter-gather query and open a cursor on every shard that contains the
        //  foreign collection.
        subpipelineExec: [2, 2]
    });

// Test that a targeted $lookup on a sharded collection can execute correctly when mongos delegates
// to a merging shard.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());

assertLookupExecution(
    pipeline, {comment: "sharded_to_sharded_on_merging_shard_targeted", allowDiskUse: true}, {
        results: expectedRes,
        // Because the $lookup stage is after a $group that requires merging, but 'allowDiskUse' is
        // true, the mongos delegates a merging shard to perform the $lookup execution.
        randomlyDelegatedMerger: true,
        // For every document that flows through the $lookup stage, the node executing the $lookup
        // will target the shard that holds the relevant data for the sharded foreign collection.
        subpipelineExec: [0, 2]
    });

// Test that an untargeted $lookup on a sharded collection can execute correctly when mongos
// delegates to a merging shard.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(reviewsColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

assertLookupExecution(
    pipeline, {comment: "sharded_to_sharded_on_merging_shard_untargeted", allowDiskUse: true}, {
        results: expectedRes,
        // Because the $lookup stage is after a $group that requires merging, but 'allowDiskUse' is
        // true, the mongos delegates a merging shard to perform the $lookup execution.
        randomlyDelegatedMerger: true,
        // For every document that flows through the $lookup stage, the node executing the $lookup
        // will perform a scatter-gather query and open a cursor on every shard that contains the
        // foreign collection.
        subpipelineExec: [2, 2]
    });

// Test that multiple top-level $lookup stages are able to be run in parallel.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(
    reviewsColl, {product_id: 1}, {product_id: "hat"}, {product_id: "hat"}, mongosDB.getName());
st.shardColl(updatesColl,
             {original_review_id: 1},
             {original_review_id: 1},
             {original_review_id: 1},
             mongosDB.getName());

pipeline = [
    {$match: {customer: "Alice"}},
    {$unwind: "$products"},
    {
        $lookup:
            {from: "reviews", localField: "products._id", foreignField: "product_id", as: "reviews"}
    },
    {$unwind: "$reviews"},
    {
        $lookup:
            {from: "updates", localField: "reviews._id", foreignField: "original_review_id", as: "updates"}
    },
    {$project: {_id: 0, "reviews._id": 1, "updates.updated_stars": 1}}
];

expectedRes = [
    {reviews: {_id: 0}, updates: [{updated_stars: 4}]},
    {reviews: {_id: 1}, updates: [{updated_stars: 1}]},
    {reviews: {_id: 2}, updates: []},
];

assertLookupExecution(pipeline, {comment: "multiple_lookups"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the $lookup stage is executed in parallel on every
    // shard that contains the local collection.
    toplevelExec: [1, 1],
    // Each node executing the $lookup will, for every document that flows through the $lookup
    // stage, target the shard(s) that holds the relevant data for the sharded foreign collection.
    subpipelineExec: [0, 2],
    // The second $lookup stage's expected execution behavior is similar to the first, executing in
    // parallel on every shard that contains the 'updates' collection and, for each node, targeting
    // shards to execute the subpipeline.
    multipleLookups: {toplevelExec: [1, 1], subpipelineExec: [0, 2], subpipelineLocalExec: [1, 0]}
});

// Test that a $lookup with a subpipeline containing a non-correlated pipeline prefix is properly
// cached in sharded environments.

// Test unsharded local collection and sharded foreign collection.
st.shardColl(reviewsColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

pipeline = [
    {$match: {customer: "Alice"}},
    {$unwind: "$products"},
    // To make sure that there is a non-correlated pipeline prefix, we will match on "name" instead
    // of _id to prevent the $match stage from being optimized before the $group.
    {$lookup: {
        from: "reviews",
        let: {customer_product_name: "$products._id"},
        pipeline: [
            {$group:
                {_id: "$product_id", avg_stars: {$avg: "$stars"}, name: {$first: "$product_id"}}
            },
            {$match: {$expr: {$eq: ["$name", "$$customer_product_name"]}}},
        ],
        as: "avg_review"}},
    {$unwind: {path: "$avg_review", preserveNullAndEmptyArrays: true}},
    {$group:
        {
            _id: "$_id",
            products: {$push: {_id: "$products._id", avg_review: "$avg_review.avg_stars"}}
        }
    }
];

expectedRes = [{_id: 0, products: [{_id: "hat", avg_review: 4.25}, {_id: "shirt", avg_review: 3}]}];

assertLookupExecution(pipeline, {comment: "unsharded_to_sharded_cache"}, {
    results: expectedRes,
    // Because the local collection is unsharded, the $lookup stage is executed on the primary
    // shard of the database.
    toplevelExec: [1, 0],
    // The node executing the $lookup will open a cursor on
    // every shard that contains the foreign collection for the first iteration of $lookup. The
    // $group stage in the subpipeline is non-correlated so the $lookup will only need to send the
    // subpipeline to each shard once to populate the cache, and will perform local queries against
    // the cache in subsequent iterations.
    subpipelineExec: [1, 1]
});

// Test sharded local collection and sharded foreign collection.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
st.shardColl(reviewsColl, {_id: 1}, {_id: 0}, {_id: 1}, mongosDB.getName());

pipeline = [
    {$unwind: "$products"},
    // To make sure that there is a non-correlated pipeline prefix, we will match on "name" instead
    // of _id to prevent the $match stage from being optimized before the $group.
    {$lookup: {
        from: "reviews",
        let: {customer_product_name: "$products._id"},
        pipeline: [
            {$group:
                {_id: "$product_id", avg_stars: {$avg: "$stars"}, name: {$first: "$product_id"}}},
            {$match: {$expr: {$eq: ["$name", "$$customer_product_name"]}}},
        ],
        as: "avg_review"}},
    {$unwind: {path: "$avg_review", preserveNullAndEmptyArrays: true}},
    {$group:
        {
            _id: "$_id",
            products: {$push: {_id: "$products._id", avg_review: "$avg_review.avg_stars"}}
        }
    }
];

expectedRes = [
    {_id: 0, products: [{_id: "hat", avg_review: 4.25}, {_id: "shirt", avg_review: 3}]},
    {_id: 1, products: [{_id: "shirt", avg_review: 3}, {_id: "bowl", avg_review: 4}]}
];

assertLookupExecution(pipeline, {comment: "sharded_to_sharded_cache"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, so the top-level stage $lookup is executed in parallel on
    // every shard that contains the local collection.
    toplevelExec: [1, 1],
    // Each node that executes the $lookup will open a cursor on every shard that contains the
    // foreign collection for the first iteration of $lookup. The $group stage in the subpipeline
    // is non-correlated so the $lookup will only need to send the subpipeline to each shard once
    // to populate the cache, and will perform local queries against the cache in subsequent
    // iterations.
    subpipelineExec: [2, 2]
});

// Test unsharded local collection and unsharded foreign collection.
assertLookupExecution(pipeline, {comment: "unsharded_to_unsharded_cache"}, {
    results: expectedRes,
    // Because the local collection is unsharded, the $lookup stage is executed on the primary
    // shard of the database.
    toplevelExec: [1, 0],
    // Because the foreign collection is unsharded, the node executing the $lookup can do a local
    // read on the foreign coll during the first iteration of $lookup. The $group stage in the
    // subpipeline is non-correlated so the $lookup will only need to do the local read once to
    // populate the cache, and will perform local queries against the cache in subsequent
    // iterations.
    subpipelineLocalExec: [1, 0],
    subpipelineExec: [0, 0]
});

// Test sharded local collection and unsharded foreign collection.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());

assertLookupExecution(pipeline, {comment: "sharded_to_unsharded_cache"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, but the foreign collection is not, so the $lookup stays
    // on the merging half of the split pipeline and executes on the primary only.
    toplevelExec: [1, 0],
    // Because the foreign collection is unsharded, the node executing the $lookup can do a local
    // read on the foreign coll during the first iteration of $lookup. The $group stage in the
    // subpipeline is non-correlated so the $lookup will only need to do the local read once to
    // populate the cache, and will perform local queries against the cache in subsequent
    // iterations.
    subpipelineLocalExec: [1, 0],
    subpipelineExec: [0, 0]
});

// Test sharded local collection and unsharded foreign collection is directed to the primary only.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());

pipeline = [
    {$unwind: "$products"},
    {
        $lookup:
            {from: "reviews", localField: "products._id", foreignField: "product_id", as: 'reviews'}
    },
    {$project: {'item': '$products._id', 'reviews.stars': 1}},
];

expectedRes = [
    {_id: 0, item: 'hat', reviews: [{stars: 4.5}, {stars: 4}]},
    {_id: 0, item: 'shirt', reviews: [{stars: 3}]},
    {_id: 1, item: 'shirt', reviews: [{stars: 3}]},
    {_id: 1, item: 'bowl', reviews: [{stars: 4}]},
];

assertLookupExecution(pipeline, {comment: "sharded_to_unsharded"}, {
    results: expectedRes,
    // The 'orders' collection is sharded, but the foreign collection is unsharded, so the $lookup
    // will stay on the merging part of the split pipeline and be sent to the primary only.
    toplevelExec: [1, 0],
    // Since it is unsharded, the foreign collection exists on the primary shard where the $lookup
    // is executing. So, the $lookup can perform local reads into the foreign collection to execute
    // the subpipelines.
    subpipelineLocalExec: [4, 0],
    subpipelineExec: [0, 0]
});

// Test sharded local collection and sharded foreign collection that becomes unsharded.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());

// Shard the collection through the stale mongos, setting it up to believe the collection is
// sharded by {product_id: 1}. Perform a query through that mongos to ensure the cache is populated.
// Then, drop the collection from the other mongos.
st.shardColl(reviewsColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());
assert.eq(0, mongosDB[reviewsColl.getName()].find().itcount());
freshReviews.drop();

// Now 'mongosDB' believes the collection is sharded, but it is actually unsharded. Test that the
// stale mongos still returns correct results.
assertLookupExecution(pipeline, {comment: "foreign_becomes_unsharded", batchSize: 1}, {
    results: expectedRes,
    // The stale mongos will believe that the foreign collection is sharded and will parallelize
    // the $lookup.
    toplevelExec: [1, 1],
    // For every document that flows through the $lookup stage, each node will send the subpipelines
    // to execute on the primary shard, since the foreign collection exists only on the primary.
    // The primary shard can execute those subpipelines as local reads.
    subpipelineLocalExec: [2, 0],
    subpipelineExec: [2, 0]
});

// Test sharded local collection and unsharded foreign collection that becomes sharded.
st.shardColl(ordersColl, {_id: 1}, {_id: 1}, {_id: 1}, mongosDB.getName());

// Perform a query through the stale mongos to ensure the cache is populated and indicates that
// the foreign collection is unsharded. Then, shard the collection from the other mongos.
assert.eq(0, mongosDB[reviewsColl.getName()].find().itcount());
assert.commandWorked(
    freshMongos.adminCommand({shardCollection: freshReviews.getFullName(), key: {_id: "hashed"}}));

// Test that the stale mongos still returns correct results.
assertLookupExecution(pipeline, {comment: "foreign_becomes_sharded", batchSize: 1}, {
    results: expectedRes,
    // The stale mongos will believe that the foreign collection is unsharded and will send the
    // $lookup to the primary only.
    toplevelExec: [1, 0],
    // For every document that flows through the $lookup stage, the node executing the $lookup will
    // perform a scatter-gather query and open a cursor on every shard that contains the foreign
    // collection.
    subpipelineExec: [4, 4]
});

st.stop();
