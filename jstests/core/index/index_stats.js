// @tags: [
//   # The test runs commands that are not allowed with security token: mapReduce.
//   not_allowed_with_signed_security_token,
//   # This test attempts to perform write operations and get index usage statistics using the
//   # $indexStats stage. The former operation must be routed to the primary in a replica set,
//   # whereas the latter may be routed to a secondary.
//   assumes_read_preference_unchanged,
//   # This test asserts on query plans expected from unsharded collections.
//   assumes_unsharded_collection,
//   does_not_support_stepdowns,
//   does_not_support_repeated_reads,
//   requires_non_retryable_writes,
//   # Uses mapReduce command.
//   requires_scripting,
//   references_foreign_collection,
// ]

let colName = "jstests_index_stats";
let col = db[colName];
col.drop();

let getUsageCount = function (indexName, collection) {
    collection = collection || col;
    let cursor = collection.aggregate([{$indexStats: {}}]);
    while (cursor.hasNext()) {
        let doc = cursor.next();

        if (doc.name === indexName) {
            return doc.accesses.ops;
        }
    }

    return undefined;
};

let getIndexKey = function (indexName) {
    let cursor = col.aggregate([{$indexStats: {}}]);
    while (cursor.hasNext()) {
        let doc = cursor.next();

        if (doc.name === indexName) {
            return doc.key;
        }
    }

    return undefined;
};

assert.commandWorked(col.insert({a: 1, b: 1, c: 1}));
assert.commandWorked(col.insert({a: 2, b: 2, c: 2}));
assert.commandWorked(col.insert({a: 3, b: 3, c: 3}));

//
// Confirm no index stats object exists prior to index creation.
//
col.findOne({a: 1});
assert.eq(undefined, getUsageCount("a_1"));

//
// Create indexes.
//
assert.commandWorked(col.createIndex({a: 1}, {name: "a_1"}));
assert.commandWorked(col.createIndex({b: 1, c: 1}, {name: "b_1_c_1"}));
let countA = 0; // Tracks expected index access for "a_1".
let countB = 0; // Tracks expected index access for "b_1_c_1".

//
// Confirm a stats object exists post index creation (with 0 count).
//
assert.eq(countA, getUsageCount("a_1"));
assert.eq({a: 1}, getIndexKey("a_1"));

//
// Confirm index stats tick on find().
//
col.findOne({a: 1});
countA++;

assert.eq(countA, getUsageCount("a_1"));

//
// Confirm index stats tick on findAndModify() update.
//
let res = db.runCommand({findAndModify: colName, query: {a: 1}, update: {$set: {d: 1}}, "new": true});
assert.commandWorked(res);
countA++;
assert.eq(countA, getUsageCount("a_1"));

//
// Confirm index stats tick on findAndModify() delete.
//
res = db.runCommand({findAndModify: colName, query: {a: 2}, remove: true});
assert.commandWorked(res);
countA++;
assert.eq(countA, getUsageCount("a_1"));
assert.commandWorked(col.insert(res.value));

//
// Confirm $and operation ticks indexes for winning plan, but not rejected plans.
//

// We cannot use explain() to determine which indexes would be used for this query, since
// 1) explain() will not bump the access counters
// 2) explain() always runs the multi planner, and the multi planner may choose a different
// index each run. We therefore run the query, and check that only one of the indexes has its
// counter bumped (assuming we never choose an index intersection plan).
const results = col.find({a: 2, b: 2}).itcount();
if (countA + 1 == getUsageCount("a_1")) {
    // Plan using index A was chosen. Index B should not have been used (assuming no index
    // intersection plans are used).
    countA++;
} else {
    // Plan using index B was chosen. Index A should not have been used (assuming no index
    // intersection plans are used).
    assert.eq(++countB, getUsageCount("b_1_c_1"));
}
assert.eq(countA, getUsageCount("a_1"));
assert.eq(countB, getUsageCount("b_1_c_1"));
assert.eq(0, getUsageCount("_id_"));

//
// Confirm index stats tick on distinct().
//
res = db.runCommand({distinct: colName, key: "b", query: {b: 1}});
assert.commandWorked(res);
countB++;
assert.eq(countB, getUsageCount("b_1_c_1"));

//
// Confirm index stats tick on aggregate w/ match.
//
res = db.runCommand({aggregate: colName, pipeline: [{$match: {b: 1}}], cursor: {}});
assert.commandWorked(res);
countB++;
assert.eq(countB, getUsageCount("b_1_c_1"));

//
// Confirm index stats tick on mapReduce with query.
//
res = db.runCommand({
    mapReduce: colName,
    map: function () {
        emit(this.b, this.c);
    },
    reduce: function (key, val) {
        return val;
    },
    query: {b: 2},
    out: {inline: 1},
});
assert.commandWorked(res);
countB++;
assert.eq(countB, getUsageCount("b_1_c_1"));

//
// Confirm index stats tick on update().
//
assert.commandWorked(col.update({a: 2}, {$set: {d: 2}}));
countA++;
assert.eq(countA, getUsageCount("a_1"));

//
// Confirm index stats tick on remove().
//
assert.commandWorked(col.remove({a: 2}));
countA++;
assert.eq(countA, getUsageCount("a_1"));

//
// Confirm multiple index $or operation ticks all involved indexes.
//
col.findOne({$or: [{a: 1}, {b: 1, c: 1}]});
countA++;
countB++;
assert.eq(countA, getUsageCount("a_1"));
assert.eq(countB, getUsageCount("b_1_c_1"));

//
// Confirm index stats object does not exist post index drop.
//
assert.commandWorked(col.dropIndex("b_1_c_1"));
countB = 0;
assert.eq(undefined, getUsageCount("b_1_c_1"));

//
// Confirm index stats object exists with count 0 once index is recreated.
//
assert.commandWorked(col.createIndex({b: 1, c: 1}, {name: "b_1_c_1"}));
assert.eq(countB, getUsageCount("b_1_c_1"));

//
// Confirm that retrieval fails if $indexStats is not in the first pipeline position.
//
assert.throws(function () {
    col.aggregate([{$match: {}}, {$indexStats: {}}]);
});

//
// Confirm index use is recorded for $lookup.
//
const foreignCollection = db[colName + "_foreign"];
foreignCollection.drop();
assert.commandWorked(foreignCollection.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
assert(col.drop());
assert.commandWorked(
    col.insert([
        {_id: 0, foreignId: 1},
        {_id: 1, foreignId: 2},
    ]),
);
assert.eq(0, getUsageCount("_id_"));
assert.eq(0, getUsageCount("_id_", foreignCollection));
let pipeline = [
    {$match: {_id: {$in: [0, 1]}}},
    {
        $lookup: {
            from: foreignCollection.getName(),
            localField: "foreignId",
            foreignField: "_id",
            as: "results",
        },
    },
];
assert.eq(2, col.aggregate(pipeline).itcount());
assert.gte(getUsageCount("_id_", col), 1, "Expected aggregation to use _id index");
assert.gte(
    getUsageCount("_id_", foreignCollection),
    1,
    "Expected aggregation to use _id index on the foreign collection",
);
//
// Confirm index use is recorded for partially pushed down pipelines with a $lookup stage
//
assert.eq(true, foreignCollection.drop());
assert.commandWorked(foreignCollection.insert([{_id: 0}, {_id: 1}, {_id: 2}]));
assert(col.drop());
assert.commandWorked(
    col.insert([
        {_id: 0, foreignId: 1},
        {_id: 1, foreignId: 2},
    ]),
);
assert.eq(0, getUsageCount("_id_"));
assert.eq(0, getUsageCount("_id_", foreignCollection));
pipeline = [
    {$match: {_id: {$in: [0, 1]}}},
    {
        $lookup: {
            from: foreignCollection.getName(),
            localField: "foreignId",
            foreignField: "_id",
            as: "results",
        },
    },
    {
        $project: {
            foreignId: 1,
            results: 1,
            matches: {$size: "$results"},
        },
    },
];
assert.eq(2, col.aggregate(pipeline).itcount());
assert.gte(getUsageCount("_id_", col), 1, "Expected aggregation to use _id index");
assert.gte(
    getUsageCount("_id_", foreignCollection),
    1,
    "Expected aggregation to use _id index on the foreign collection",
);

//
// Confirm index use is recorded for $graphLookup.
//
foreignCollection.drop();
assert.commandWorked(
    foreignCollection.insert([
        {_id: 0, connectedTo: 1},
        {_id: 1, connectedTo: "X"},
        {_id: 2, connectedTo: 3},
        {_id: 3, connectedTo: "Y"}, // Be sure to use a different value here to make sure
        // $graphLookup doesn't cache the query.
    ]),
);
assert(col.drop());
assert.commandWorked(
    col.insert([
        {_id: 0, foreignId: 0},
        {_id: 1, foreignId: 2},
    ]),
);
assert.eq(0, getUsageCount("_id_"));
assert.eq(
    2,
    col
        .aggregate([
            {$match: {_id: {$in: [0, 1]}}},
            {
                $graphLookup: {
                    from: foreignCollection.getName(),
                    startWith: "$foreignId",
                    connectToField: "_id",
                    connectFromField: "connectedTo",
                    as: "results",
                },
            },
        ])
        .itcount(),
);
assert.gte(getUsageCount("_id_", col), 1, "Expected aggregation to use _id index");
assert.eq(
    2 * 3,
    getUsageCount("_id_", foreignCollection),
    "Expected each of two graph searches to issue 3 queries, each using the _id index",
);

//
// Confirm that index 'hidden' status can be found in '$indexStats'.
//
assert(col.drop());
assert.commandWorked(col.createIndex({a: 1}, {hidden: true}));
const hiddenIndexStats = col.aggregate([{$indexStats: {}}, {$match: {"name": "a_1", "spec.hidden": true}}]).toArray();
assert.eq(hiddenIndexStats.length, 1);

//
// Confirm the index usage stats will be reset on hiding and unhiding the index.
//
assert(col.drop());
assert.commandWorked(col.insert({a: 1}));
assert.commandWorked(col.createIndex({a: 1}));

// When the index is not hidden, the stats keep counting.
res = col.findOne({a: 1});
assert(1, res);
assert.eq(1, getUsageCount("a_1"));

// On modifying an index's definition (i.e. hiding or unhiding an index), the usage stats of the
// index will be reset to zero.
assert.commandWorked(col.hideIndex("a_1"));
assert.eq(0, getUsageCount("a_1"));

// Confirm that the stats don't tick after the index is hidden.
res = col.findOne({a: 1});
assert(1, res);
assert.eq(0, getUsageCount("a_1"));

assert.commandWorked(col.unhideIndex("a_1"));
res = col.findOne({a: 1});
assert(1, res);
assert.eq(1, getUsageCount("a_1"));
