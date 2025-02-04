/**
 * Checks that caching a query with predicate X, followed by a similar query with X in a rooted $or
 * does not pass along the isCountLike attribute to the subquery. Previously we would always pass
 * isCountLike to the child, meaning we could retrieve an incorrect plan from the cache and tassert.
 *
 * @tags: [
 *   # If all chunks are moved off of a shard, it can cause the plan cache to miss commands.
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   assumes_unsharded_collection,
 *   not_allowed_with_signed_security_token,
 * ]
 */
const collName = "is_count_like";
const coll = db[collName];
coll.drop();

// Creating this index allows an isCountLike index plan to be turned into a COUNT_SCAN, reproducing
// the issue.
coll.createIndex({a: 1});
coll.createIndex({b: 1});
coll.createIndex({a: 1, b: 1});

coll.insert({a: 0, b: 0});

function testConsecutiveQueriesWork(firstQuery, secondQuery) {
    // Run each query enough to get them cached. Then swap the order, to make sure there are no
    // failures if the second query is cached first.
    coll.getPlanCache().clear();
    for (let i = 0; i < 5; i++) {
        firstQuery();
    }
    for (let i = 0; i < 5; i++) {
        secondQuery();
    }

    coll.getPlanCache().clear();
    for (let i = 0; i < 5; i++) {
        secondQuery();
    }
    for (let i = 0; i < 5; i++) {
        firstQuery();
    }
}

function testCountLikeStage(stage) {
    testConsecutiveQueriesWork(
        function() {
            coll.aggregate([{$match: {a: 0}}, stage]).toArray();
        },
        function() {
            coll.aggregate([{$match: {$or: [{a: 0}, {b: 0}]}}, stage]).toArray();
        });
}

testConsecutiveQueriesWork(
    function() {
        coll.find({a: 0}).count();
    },
    function() {
        coll.find({$or: [{a: 0}, {b: 0}]}).count();
    });
// Replacing root with {} and $count are both count-like as well.
testCountLikeStage({$replaceRoot: {newRoot: {}}});
testCountLikeStage({$count: "c"});
