/**
 * Tests if the $changeStreamSplitLargeEvent stage can be used together with other stages allowed in
 * change stream pipeline.
 */
(function() {
"use strict";

const testDB = db.getSiblingDB("test");
const testColl = testDB[jsTestName()];

assert.commandWorked(
    db.adminCommand({appendOplogNote: 1, data: {msg: "dummy write to advance cluster time"}}));

// Capture cluster time before any events.
const startClusterTime = testDB.hello().$clusterTime.clusterTime;

// Ensure there is at least one event for the test to run faster.
testColl.insertOne({first_name: "Maya", last_name: "Ryan", homework: [10, 5, 10], pets: {cats: 2}});

// Define an instance of every stage allowed in a change stream pipeline. The instances must not
// filter out our only event when added to the change stream pipeline.
const allowedStages = [
    {$addFields: {totalHomework: {$sum: "$fullDocument.homework"}}},
    {$match: {"fullDocument.first_name": "Maya"}},
    {$project: {"fullDocument.first_name": 1, "fullDocument.last_name": 1}},
    {
        $replaceRoot: {
            newRoot: {
                full_name: {$concat: ["$fullDocument.first_name", " ", "$fullDocument.last_name"]},
                _id: "$_id"
            }
        }
    },
    {
        $replaceWith: {
            _id: "$_id",
            pets: {$mergeObjects: [{dogs: 0, cats: 0, birds: 0, fish: 0}, "$fullDocument.pets"]}
        }
    },
    {$redact: {$cond: {if: {$eq: ["$level", 2]}, then: "$$PRUNE", else: "$$DESCEND"}}},
    {$set: {totalHomework: {$sum: "$fullDocument.homework"}}},
    {$unset: ["fullDocument.first_name", "fullDocument.last_name"]},
];

for (const stage of allowedStages) {
    const changeStreamPipeline = [stage, {$changeStreamSplitLargeEvent: {}}];
    const changeStreamCursor =
        testColl.watch(changeStreamPipeline, {startAtOperationTime: startClusterTime});
    assert.soon(
        () => changeStreamCursor.hasNext(),
        "Unexpected lack of events for the change stream pipeline " + tojson(changeStreamPipeline));
    changeStreamCursor.close();
}
})();
