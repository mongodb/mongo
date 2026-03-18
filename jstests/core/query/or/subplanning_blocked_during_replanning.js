/**
 * Ensure that replanning a rooted $or query does not trigger subplanning.
 * TODO SERVER-120492: Delete this test if the subplanning restriction can be lifted.
 * @tags: [requires_getmore]
 */
const collName = jsTestName();
const coll = db[collName];
coll.drop();
coll.insert({a: 2, b: 3});

coll.createIndex({a: 1});
coll.createIndex({b: 1});

function runQuery() {
    coll.find({$or: [{a: 2}, {b: 3}]})
        .sort({_id: 1})
        .toArray();
}

// Cache the plan.
runQuery();
runQuery();

// Adding more documents will trigger replanning for the same query since
// the number of works needed will drastically increase from the value
// that's stored in the cache.
const docs = [];
for (let i = 0; i < 10000; i++) {
    docs.push({a: 2, b: 3});
}
coll.insertMany(docs);

// Trigger replanning, ensuring that it succeeds.
runQuery();
