/**
 * Tests that the $$NOW and $$CLUSTER_TIME system variables can be used when performing updates on a
 * replica set.
 *
 * Tag this test as 'requires_find_command' to prevent it from running in the legacy passthroughs.
 * The 'requires_replication' tag prevents the test from running on variants with storage options
 * which cannot support a replica set.
 * @tags: [requires_find_command, requires_replication]
 */
(function() {
"use strict";

const rst = new ReplSetTest({name: jsTestName(), nodes: 1});
rst.startSet();
rst.initiate();

const db = rst.getPrimary().getDB(jsTestName());
const otherColl = db.other;
const coll = db.test;
otherColl.drop();
coll.drop();

// Insert N docs, with the _id field set to the current Date. We sleep for a short period
// between insertions, such that the Date value increases for each successive document.
let bulk = coll.initializeUnorderedBulkOp();
const _idStart = new Date();
const numDocs = 10;
for (let i = 0; i < numDocs; ++i) {
    bulk.insert({_id: new Date(), insertClusterTime: new Timestamp(0, 0)});
    if (i < numDocs - 1) {
        sleep(100);
    }
}
const _idEnd = new Date();

assert.commandWorked(bulk.execute());

// Test that $$NOW and $$CLUSTER_TIME are available and remain constant across all updated
// documents.
let writeResult =
    assert.commandWorked(coll.update({$where: "sleep(10); return true"},
                                     [{$addFields: {now: "$$NOW", ctime: "$$CLUSTER_TIME"}}],
                                     {multi: true}));

assert.eq(writeResult.nMatched, numDocs);
assert.eq(writeResult.nModified, numDocs);

let results = coll.find().toArray();
assert.eq(results.length, numDocs);
assert(results[0].now instanceof Date);
assert(results[0].ctime instanceof Timestamp);
for (let result of results) {
    assert.eq(result.now, results[0].now);
    assert.eq(result.ctime, results[0].ctime);
}

// Test that $$NOW and $$CLUSTER_TIME advance between updates but remain constant across all
// updates in a given batch.
writeResult = assert.commandWorked(db.runCommand({
    update: coll.getName(),
    updates: [
        {
            q: {$where: "sleep(10); return true"},
            u: [{$addFields: {now2: "$$NOW", ctime2: "$$CLUSTER_TIME"}}],
            multi: true
        },
        {
            q: {$where: "sleep(10); return true"},
            u: [{$addFields: {now3: "$$NOW", ctime3: "$$CLUSTER_TIME"}}],
            multi: true
        }
    ]
}));

assert.eq(writeResult.n, numDocs * 2);
assert.eq(writeResult.nModified, numDocs * 2);

results = coll.find().toArray();
assert.eq(results.length, numDocs);
assert(results[0].now2 instanceof Date);
assert(results[0].ctime2 instanceof Timestamp);
for (let result of results) {
    // The now2 and ctime2 fields are greater than the values from the previous update.
    assert.gt(result.now2, result.now);
    assert.gt(result.ctime2, result.ctime);
    // The now2 and ctime2 fields are the same across all documents.
    assert.eq(result.now2, results[0].now2);
    assert.eq(result.ctime2, results[0].ctime2);
    // The now2 and ctime2 fields are the same as now3 and ctime3 across all documents.
    assert.eq(result.now2, result.now3);
    assert.eq(result.ctime2, result.ctime3);
}

// Test that $$NOW and $$CLUSTER_TIME can be used in the query portion of an update.
const _idMidpoint = new Date(_idStart.getTime() + (_idEnd.getTime() - _idStart.getTime()) / 2);
writeResult =
    assert.commandWorked(coll.update({
        $expr: {
            $and: [
                {$lt: ["$_id", {$min: [_idMidpoint, "$$NOW"]}]},
                {$gt: ["$$CLUSTER_TIME", "$insertClusterTime"]}
            ]
        }
    },
                                     [{$addFields: {now4: "$$NOW", ctime4: "$$CLUSTER_TIME"}}],
                                     {multi: true}));

assert.lt(writeResult.nMatched, numDocs);
assert.lt(writeResult.nModified, numDocs);

results = coll.find().sort({_id: 1}).toArray();
assert.eq(results.length, numDocs);
assert(results[0].now4 instanceof Date);
assert(results[0].ctime4 instanceof Timestamp);
for (let result of results) {
    if (result._id.getTime() < _idMidpoint.getTime()) {
        assert.eq(result.now4, results[0].now4);
        assert.eq(result.ctime4, results[0].ctime4);
        assert.gt(result.now4, result.now3);
        assert.gt(result.ctime4, result.ctime3);
    } else {
        assert.eq(result.now4, undefined);
        assert.eq(result.ctime4, undefined);
    }
}

// Test that we can explain() an update command that uses $$NOW and $$CLUSTER_TIME.
assert.commandWorked(
    coll.explain().update(
        {
            $expr: {
                $and: [
                    {$lt: ["$_id", {$min: [_idMidpoint, "$$NOW"]}]},
                    {$gt: ["$$CLUSTER_TIME", "$insertClusterTime"]}
                ]
            }
        },
        [{$addFields: {explainDoesNotWrite1: "$$NOW", explainDoesNotWrite2: "$$CLUSTER_TIME"}}],
        {multi: true}));

// Test that $$NOW and $$CLUSTER_TIME can be used when issuing updates via the Bulk API, and
// remain constant across all updates within a single bulk operation.
// TODO SERVER-41174: Note that if the bulk update operation exceeds the maximum BSON command
// size, it may issue two or more separate update commands. $$NOW and $$CLUSTER_TIME will be
// constant within each update command, but not across commands.
bulk = coll.initializeUnorderedBulkOp();
bulk.find({$where: "sleep(10); return true"}).update([
    {$addFields: {now5: "$$NOW", ctime5: "$$CLUSTER_TIME"}}
]);
bulk.find({$where: "sleep(10); return true"}).update([
    {$addFields: {now6: "$$NOW", ctime6: "$$CLUSTER_TIME"}}
]);
writeResult = assert.commandWorked(bulk.execute());

assert.eq(writeResult.nMatched, numDocs * 2);
assert.eq(writeResult.nModified, numDocs * 2);

results = coll.find().toArray();
assert.eq(results.length, numDocs);
assert(results[0].now5 instanceof Date);
assert(results[0].ctime5 instanceof Timestamp);
for (let result of results) {
    // The now5 and ctime5 fields are the same across all documents.
    assert.eq(result.now5, results[0].now5);
    assert.eq(result.ctime5, results[0].ctime5);
    // The now5 and ctime5 fields are the same as now6 and ctime6 across all documents.
    assert.eq(result.now5, result.now6);
    assert.eq(result.ctime5, result.ctime6);
}

// Test that $$NOW and $$CLUSTER_TIME can be used in a findAndModify query and update.
let returnedDoc = coll.findAndModify({
    query: {
        $expr: {
            $and: [
                {$lt: ["$_id", {$min: [_idMidpoint, "$$NOW"]}]},
                {$gt: ["$$CLUSTER_TIME", "$insertClusterTime"]}
            ]
        }
    },
    update: [{$addFields: {nowFAM: "$$NOW", ctimeFAM: "$$CLUSTER_TIME"}}],
    sort: {_id: 1},
    new: true
});
assert(returnedDoc.nowFAM instanceof Date);
assert(returnedDoc.ctimeFAM instanceof Timestamp);
assert.gt(returnedDoc.nowFAM, returnedDoc.now4);
assert.gt(returnedDoc.ctimeFAM, returnedDoc.ctime4);

results = coll.find({nowFAM: {$exists: true}, ctimeFAM: {$exists: true}}).toArray();
assert.eq(results.length, 1);
assert.docEq(results[0], returnedDoc);

// Test that $$NOW and $$CLUSTER_TIME can be used in a findAndModify upsert.
returnedDoc = coll.findAndModify({
    query: {fieldDoesNotExist: {$exists: true}},
    update: [{$addFields: {_id: "$$NOW", nowFAMUpsert: "$$NOW", ctimeFAMUpsert: "$$CLUSTER_TIME"}}],
    sort: {_id: 1},
    upsert: true,
    new: true
});
assert(returnedDoc.nowFAMUpsert instanceof Date);
assert(returnedDoc.ctimeFAMUpsert instanceof Timestamp);

assert.eq(coll.find().itcount(), numDocs + 1);
results = coll.find({nowFAMUpsert: {$exists: true}, ctimeFAMUpsert: {$exists: true}}).toArray();
assert.eq(results.length, 1);
assert.docEq(results[0], returnedDoc);

// Test that $$NOW and $$CLUSTER_TIME can be used in a findAndModify delete.
returnedDoc = coll.findAndModify({
    query: {
        nowFAMUpsert: {$exists: true},
        ctimeFAMUpsert: {$exists: true},
        $expr: {
            $and:
                [{$lt: ["$nowFAMUpsert", "$$NOW"]}, {$gt: ["$$CLUSTER_TIME", "$ctimeFAMUpsert"]}]
        }
    },
    sort: {_id: 1},
    remove: true
});
assert.eq(coll.find({nowFAMUpsert: {$exists: true}}).itcount(), 0);
assert.eq(coll.find().itcount(), numDocs);
assert.neq(returnedDoc, null);

// Test that we can explain() a findAndModify command that uses $$NOW and $$CLUSTER_TIME.
assert.commandWorked(coll.explain().findAndModify({
    query: {
        $expr: {
            $and: [
                {$lt: ["$_id", {$min: [_idMidpoint, "$$NOW"]}]},
                {$gt: ["$$CLUSTER_TIME", "$insertClusterTime"]}
            ]
        }
    },
    update: [{$addFields: {explainDoesNotWrite1: "$$NOW", explainDoesNotWrite2: "$$CLUSTER_TIME"}}],
    sort: {_id: 1},
    new: true
}));

// Test that we can use $$NOW and $$CLUSTER_TIME in an update via a $merge aggregation. We first
// use $merge to copy the current contents of 'coll' into 'otherColl'.
assert.commandWorked(db.createCollection(otherColl.getName()));
assert.doesNotThrow(
    () => coll.aggregate(
        [{$merge: {into: otherColl.getName(), whenMatched: "fail", whenNotMatched: "insert"}}]));
// Run an aggregation which adds $$NOW and $$CLUSTER_TIME fields into the pipeline document,
// then do the same to the documents in the output collection via a pipeline update.
assert.doesNotThrow(() => coll.aggregate([
    {$addFields: {aggNow: "$$NOW", aggCT: "$$CLUSTER_TIME"}},
    {
        $merge: {
            into: otherColl.getName(),
            let : {aggNow: "$aggNow", aggCT: "$aggCT"},
            whenMatched: [{
                $addFields: {
                    aggNow: "$$aggNow",
                    aggCT: "$$aggCT",
                    mergeNow: "$$NOW",
                    mergeCT: "$$CLUSTER_TIME"
                }
            }],
            whenNotMatched: "fail"
        }
    }
]));
// Verify that the agg pipeline's $$NOW and $$CLUSTER_TIME match the $merge update pipeline's.
results = otherColl.find().toArray();
assert.eq(results.length, numDocs);
assert(results[0].mergeNow instanceof Date);
assert(results[0].mergeCT instanceof Timestamp);
for (let result of results) {
    // The mergeNow and mergeCT fields are greater than the values from the previous updates.
    assert.gt(result.mergeNow, result.now5);
    assert.gt(result.mergeCT, result.ctime5);
    // The mergeNow and mergeCT fields are the same across all documents.
    assert.eq(result.mergeNow, results[0].mergeNow);
    assert.eq(result.mergeCT, results[0].mergeCT);
    // The mergeNow and mergeCT fields are the same as aggNow and aggCT across all documents.
    assert.eq(result.mergeNow, result.aggNow);
    assert.eq(result.mergeCT, result.aggCT);
}

rst.stopSet();
}());
