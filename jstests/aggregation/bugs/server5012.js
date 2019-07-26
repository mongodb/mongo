(function() {
"use strict";
load('jstests/aggregation/data/articles.js');

const article = db.getSiblingDB("aggdb").getCollection("article");
const cursor = article.aggregate(
    [{$sort: {_id: 1}}, {$project: {author: 1, _id: 0}}, {$project: {Writer: "$author"}}]);
const expected = [{Writer: "bob"}, {Writer: "dave"}, {Writer: "jane"}];

assert.eq(cursor.toArray(), expected);
}());
