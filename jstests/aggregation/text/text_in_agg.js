// SERVER-11675 Text search integration with aggregation
import {assertErrorCode} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];
coll.drop();

assert.commandWorked(coll.insert({_id: 1, text: "apple", words: 1}));
assert.commandWorked(coll.insert({_id: 2, text: "banana", words: 1}));
assert.commandWorked(coll.insert({_id: 3, text: "apple banana", words: 2}));
assert.commandWorked(coll.insert({_id: 4, text: "cantaloupe", words: 1}));

assert.commandWorked(coll.createIndex({text: "text"}));

// query should have subfields query, project, sort, skip and limit. All but query are optional.
const assertSameAsFind = function (query) {
    let cursor = coll.find(query.query);
    const pipeline = [{$match: query.query}];

    if ("project" in query) {
        cursor = coll.find(query.query, query.project); // no way to add to constructed cursor
        pipeline.push({$project: query.project});
    }

    if ("sort" in query) {
        cursor = cursor.sort(query.sort);
        pipeline.push({$sort: query.sort});
    }

    if ("skip" in query) {
        cursor = cursor.skip(query.skip);
        pipeline.push({$skip: query.skip});
    }

    if ("limit" in query) {
        cursor = cursor.limit(query.limit);
        pipeline.push({$limit: query.limit});
    }

    const findRes = cursor.toArray();
    const aggRes = coll.aggregate(pipeline).toArray();

    // If the query doesn't specify its own sort, there is a possibility that find() and
    // aggregate() will return the same results in different orders. We sort by _id on the
    // client side, so that the results still count as equal.
    if (!query.hasOwnProperty("sort")) {
        findRes.sort(function (a, b) {
            return a._id - b._id;
        });
        aggRes.sort(function (a, b) {
            return a._id - b._id;
        });
    }

    assert.docEq(aggRes, findRes);
};

assertSameAsFind({query: {}}); // sanity check
assertSameAsFind({query: {$text: {$search: "apple"}}});
assertSameAsFind({query: {_id: 1, $text: {$search: "apple"}}});
assertSameAsFind({query: {$text: {$search: "apple"}}, project: {_id: 1, score: {$meta: "textScore"}}});
assertSameAsFind({query: {$text: {$search: "apple banana"}}, project: {_id: 1, score: {$meta: "textScore"}}});
assertSameAsFind({
    query: {$text: {$search: "apple banana"}},
    project: {_id: 1, score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
});
assertSameAsFind({
    query: {$text: {$search: "apple banana"}},
    project: {_id: 1, score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
    limit: 1,
});
assertSameAsFind({
    query: {$text: {$search: "apple banana"}},
    project: {_id: 1, score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
    skip: 1,
});
assertSameAsFind({
    query: {$text: {$search: "apple banana"}},
    project: {_id: 1, score: {$meta: "textScore"}},
    sort: {score: {$meta: "textScore"}},
    skip: 1,
    limit: 1,
});

// $meta sort specification should be rejected if it has additional keys.
assert.throws(function () {
    coll.aggregate([
        {$match: {$text: {$search: "apple banana"}}},
        {$sort: {textScore: {$meta: "textScore", extra: 1}}},
    ]).itcount();
});

// $meta sort specification should be rejected if the type of meta sort is not known.
assert.throws(function () {
    coll.aggregate([{$match: {$text: {$search: "apple banana"}}}, {$sort: {textScore: {$meta: "unknown"}}}]).itcount();
});

// Sort specification should be rejected if a $-keyword other than $meta is used.
assert.throws(function () {
    coll.aggregate([
        {$match: {$text: {$search: "apple banana"}}},
        {$sort: {textScore: {$notMeta: "textScore"}}},
    ]).itcount();
});

// Sort specification should be rejected if it is a string, not an object with $meta.
assert.throws(function () {
    coll.aggregate([{$match: {$text: {$search: "apple banana"}}}, {$sort: {textScore: "textScore"}}]).itcount();
});

// sharded find requires projecting the score to sort, but sharded agg does not.
let findRes = coll
    .find({$text: {$search: "apple banana"}}, {textScore: {$meta: "textScore"}})
    .sort({textScore: {$meta: "textScore"}})
    .map(function (obj) {
        delete obj.textScore; // remove it to match agg output
        return obj;
    });
let res = coll
    .aggregate([{$match: {$text: {$search: "apple banana"}}}, {$sort: {textScore: {$meta: "textScore"}}}])
    .toArray();
assert.eq(res, findRes);

// Make sure {$meta: 'textScore'} can be used as a sub-expression
res = coll
    .aggregate([
        {$match: {_id: 1, $text: {$search: "apple"}}},
        {
            $project: {
                words: 1,
                score: {$meta: "textScore"},
                wordsTimesScore: {$multiply: ["$words", {$meta: "textScore"}]},
            },
        },
    ])
    .toArray();
assert.eq(res[0].wordsTimesScore, res[0].words * res[0].score, tojson(res));

// And can be used in $group
res = coll
    .aggregate([
        {$match: {_id: 1, $text: {$search: "apple banana"}}},
        {$group: {_id: {$meta: "textScore"}, score: {$first: {$meta: "textScore"}}}},
    ])
    .toArray();
assert.eq(res[0]._id, res[0].score, tojson(res));

// Make sure metadata crosses shard -> merger boundary
res = coll
    .aggregate([
        {$match: {_id: 1, $text: {$search: "apple"}}},
        {$project: {scoreOnShard: {$meta: "textScore"}}},
        {$limit: 1}, // force a split. later stages run on merger
        {$project: {scoreOnShard: 1, scoreOnMerger: {$meta: "textScore"}}},
    ])
    .toArray();
assert.eq(res[0].scoreOnMerger, res[0].scoreOnShard);
let score = res[0].scoreOnMerger; // save for later tests

// Make sure metadata crosses shard -> merger boundary even if not used on shard
res = coll
    .aggregate([
        {$match: {_id: 1, $text: {$search: "apple"}}},
        {$limit: 1}, // force a split. later stages run on merger
        {$project: {scoreOnShard: 1, scoreOnMerger: {$meta: "textScore"}}},
    ])
    .toArray();
assert.eq(res[0].scoreOnMerger, score);

// Make sure metadata works if first $project doesn't use it.
res = coll
    .aggregate([
        {$match: {_id: 1, $text: {$search: "apple"}}},
        {$project: {_id: 1}},
        {$project: {_id: 1, score: {$meta: "textScore"}}},
    ])
    .toArray();
assert.eq(res[0].score, score);

// Make sure the pipeline fails if it tries to reference the text score and it doesn't exist.
res = coll.runCommand({aggregate: coll.getName(), pipeline: [{$project: {_id: 1, score: {$meta: "textScore"}}}]});
assert.commandFailed(res);

// Make sure the metadata is 'missing()' when it doesn't exist because the document changed
res = coll
    .aggregate([
        {$match: {_id: 1, $text: {$search: "apple banana"}}},
        {$group: {_id: 1, score: {$first: {$meta: "textScore"}}}},
        {$project: {_id: 1, scoreAgain: {$meta: "textScore"}}},
    ])
    .toArray();
assert(!("scoreAgain" in res[0]));

// Make sure metadata works after a $unwind
assert.commandWorked(coll.insert({_id: 5, text: "mango", words: [1, 2, 3]}));
res = coll
    .aggregate([
        {$match: {$text: {$search: "mango"}}},
        {$project: {score: {$meta: "textScore"}, _id: 1, words: 1}},
        {$unwind: "$words"},
        {$project: {scoreAgain: {$meta: "textScore"}, score: 1}},
    ])
    .toArray();
assert.eq(res[0].scoreAgain, res[0].score);

// Error checking
// $match, but wrong position
assertErrorCode(coll, [{$sort: {text: 1}}, {$match: {$text: {$search: "apple banana"}}}], 17313);

// wrong $stage, but correct position
assertErrorCode(coll, [{$project: {searchValue: {$text: {$search: "apple banana"}}}}], 31325);
assertErrorCode(coll, [{$sort: {$text: {$search: "apple banana"}}}], 17312);
