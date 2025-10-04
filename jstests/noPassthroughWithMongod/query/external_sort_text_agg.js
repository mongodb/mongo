// Ensure text search metadata is not lost in an external sort
let t = db.external_sort_text_agg;
t.drop();
t.createIndex({text: "text"});
for (let i = 0; i < 100; i++) {
    t.insert({_id: i, text: "asdf ".repeat(209999)});
    // string over 1MB to hit the 100MB threshold for external sort
}

let score = t.find({$text: {$search: "asdf"}}, {score: {$meta: "textScore"}}).next().score;
let res = t.aggregate(
    [
        {$match: {$text: {$search: "asdf"}}},
        {$sort: {"_id": 1}},
        {$project: {string: "$text", score: {$meta: "textScore"}}},
    ],
    {allowDiskUse: true},
);
// we must use .next() rather than a $limit because a $limit will optimize away the external sort
printjson(res.next());
assert.eq(res.next().score, score);
