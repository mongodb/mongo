/**
 * Test that $textScore is accessible as $score in find and findAndModify queries.
 *
 * @tags: [
 *   featureFlagRankFusionFull,
 *   requires_fcv_81,
 *   # Ban in any configurations that require retryable writes. Although findAndModify is a
 *   # retryable write command, the 'fields' option does not currently work with retryable writes.
 *   requires_non_retryable_writes,
 * ]
 */
import {resultsEq} from "jstests/aggregation/extras/utils.js";

const coll = db[jsTestName()];

function setUpCollection() {
    coll.drop();

    assert.commandWorked(coll.insertMany([
        {_id: 0, x: "test"},
        {_id: 1, x: "hello"},
        {_id: 2, x: "test test"},
    ]));

    assert.commandWorked(coll.createIndex({x: "text"}));
}

const textMatchExpression = {
    $text: {$search: "test"}
};

(function testFindProjectScore() {
    setUpCollection();
    const results = coll.find(textMatchExpression, {x: 1, score: {$meta: "score"}}).toArray();
    assert(resultsEq(results,
                     [{_id: 0, x: "test", score: 1.1}, {_id: 2, x: "test test", score: 1.5}]));
})();

(function testFindSortOnScore() {
    setUpCollection();
    const results =
        coll.find(textMatchExpression, {_id: 1}).sort({score: {$meta: "score"}}).toArray();
    assert.eq(results, [{_id: 2}, {_id: 0}]);
})();

(function testFindAndModifyRemoveSortAndProjectScore() {
    setUpCollection();
    const result = coll.findAndModify({
        query: textMatchExpression,
        fields: {score: {$meta: "score"}},
        sort: {score: {$meta: "score"}},
        remove: true
    });
    assert.eq(result, {_id: 2, x: "test test", score: 1.5});
})();

(function testFindAndModifyRemoveSortOnScore() {
    setUpCollection();
    const result = coll.findAndModify(
        {query: textMatchExpression, sort: {score: {$meta: "score"}}, remove: true});
    assert.eq(result, {_id: 2, x: "test test"});
})();

(function testFindAndModifyUpdateSortAndProjectScore() {
    setUpCollection();
    const result = coll.findAndModify({
        query: textMatchExpression,
        fields: {score: {$meta: "score"}},
        sort: {score: {$meta: "score"}},
        update: [{$set: {a: 1}}],
        new: true
    });
    assert.eq(result, {_id: 2, x: "test test", a: 1, score: 1.5});
})();

(function testFindAndModifyUpdateSortOnScore() {
    setUpCollection();
    const result = coll.findAndModify({
        query: textMatchExpression,
        sort: {score: {$meta: "score"}},
        update: [{$set: {a: 1}}],
        new: true
    });
    assert.eq(result, {_id: 2, x: "test test", a: 1});
})();