/**
 * Pushing down computed projection before sort+limit can cause the projection to be evaluated on
 * invalid documents and throw exceptions. This test checks that we do not push computed projection
 * before sort+limit, preventing valid queries from failing.
 *
 * This test was intended to reproduce a bug, SERVER-54128.
 */
(function() {

const coll = db.expression_swap_limit;
coll.drop();

const NUM_INVALID_DOCS = 10;

const docs = Array.from({length: NUM_INVALID_DOCS}, (_, i) => ({_id: i, a: ""}));
docs.push({_id: 99, a: 123});
coll.insert(docs);

coll.createIndex({some_other_field: 1});

const predicate = {
    $or: [
        // One branch of the $or needs to have more than one relevant index.
        {_id: {$lt: 9999}, some_other_field: {$ne: 3}},
        // The other branch doesn't matter: it's only here to prevent the $or being
        // optimized out.
        {this_predicate_matches_nothing: true},
    ]
};
const sortSpec = {
    _id: -1
};
const oppositeSortSpec = {
    _id: +1
};
const projection = {
    _id: 1,
    b: {$round: "$a"},
};
const pipeline1 = [
    // We need a rooted $or to trigger the SubplanStage.
    // Instead of one MultiPlanStage for the whole query, we get one
    // per branch of the $or.
    {$match: predicate},
    // Next we need a $sort+$limit. These two stages unambiguously select _id: 99.
    // From the user's point of view, the $limit returns a bag of 1 document.
    {$sort: sortSpec},
    {$limit: 1},
    // This projection raises an error if we evaluate it on any document other than
    // _id: 99, because that is the only document where 'a' is numeric.
    {$project: projection},
];
{
    // The pipeline should succeed.
    const aggResult = coll.aggregate(pipeline1).toArray();
    assert.docEq(aggResult, [{_id: 99, b: 123}]);

    // The pipeline should succeed without pushing down to find.
    const noOptResult =
        coll.aggregate([{$_internalInhibitOptimization: {}}].concat(pipeline1)).toArray();
    assert.docEq(noOptResult, [{_id: 99, b: 123}]);
}

// Similarly, we can select the 1 valid document by flipping the sort and skipping
// all but one document.
const pipeline2 = [
    {$match: predicate},
    {$sort: oppositeSortSpec},
    {$skip: NUM_INVALID_DOCS},
    {$project: projection},
];
{
    // The pipeline should succeed.
    const aggResult = coll.aggregate(pipeline2).toArray();
    assert.docEq(aggResult, [{_id: 99, b: 123}]);

    // The pipeline should succeed without pushing down to find.
    const noOptResult =
        coll.aggregate([{$_internalInhibitOptimization: {}}].concat(pipeline2)).toArray();
    assert.docEq(noOptResult, [{_id: 99, b: 123}]);
}
})();
