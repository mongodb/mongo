/**
 * Tests how regex filters are satisfied with different index + query collation combinations
 * @tags: [
 *   requires_fcv_80,
 * ]
 */

import {
    getPlanStage,
    getWinningPlanFromExplain,
    isCollscan,
    isIxscan
} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js"

function assertIXScanTightBounds(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isIxscan(db, winningPlan));
    const ixscan = getPlanStage(winningPlan, 'IXSCAN');
    assert.doesNotContain("[\"\", {})", ixscan.indexBounds.value, "Unexpected full IXSCAN plan!");
}

const collName = jsTestName();

assertDropAndRecreateCollection(db, collName);
const collection = db[collName];

assert.commandWorked(collection.insertMany([
    {value: "c"},
    {value: "d"},
    {value: "ç"}
]));  // Simple collation sorting would produce c, d, ç, French would produce c, ç, d

{
    jsTestLog("Testing regex filters with simple collation on index with simple collation too...");

    assert.commandWorked(collection.createIndex({value: 1}));

    jsTestLog("Testing that prefix regexes use a bounded index scan plan");

    let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1}).toArray();
    assert.sameMembers([{value: "c"}], results);

    let explain = collection.find({value: {$regex: "^c"}}).explain();
    jsTestLog(explain);
    assertIXScanTightBounds(explain);

    jsTestLog("Testing that non prefix regexes use collscan");

    results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1}).toArray();
    assert.sameMembers([{value: "c"}], results);

    explain = collection.find({value: {$regex: "c"}}).explain();
    jsTestLog(explain);
    let winningPlan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, winningPlan));
}

{
    jsTestLog("Testing regex filters with not simple collation on index with simple collation...");
    assert.commandWorked(collection.dropIndex({value: 1}));
    assert.commandWorked(collection.createIndex({value: 1}));

    jsTestLog("Testing that prefix regexes use a bounded index scan plan");

    let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1})
                      .collation({locale: "fr"})
                      .toArray();
    assert.sameMembers([{value: "c"}], results);

    let explain = collection.find({value: {$regex: "^c"}}).collation({locale: "fr"}).explain();
    jsTestLog(explain);
    assertIXScanTightBounds(explain);

    jsTestLog("Testing that non prefix regexes use collscan");

    results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1})
                  .collation({locale: "fr"})
                  .toArray();
    assert.sameMembers([{value: "c"}], results);

    explain = collection.find({value: {$regex: "c"}}).collation({locale: "fr"}).explain();
    jsTestLog(explain);
    let winningPlan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, winningPlan));
}

{
    jsTestLog("Testing regex filters with simple collation on index with not simple collation...");
    assert.commandWorked(collection.dropIndex({value: 1}));
    assert.commandWorked(collection.createIndex({value: 1}, {collation: {locale: "fr"}}));

    jsTestLog("Testing that prefix regexes use collscan");

    let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1}).toArray();
    assert.sameMembers([{value: "c"}], results);

    let explain = collection.find({value: {$regex: "^c"}}).explain();
    jsTestLog(explain);
    let winningPlan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, winningPlan));

    jsTestLog("Testing that non prefix regexes use collscan");

    results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1}).toArray();
    assert.sameMembers([{value: "c"}], results);

    explain = collection.find({value: {$regex: "c"}}).explain();
    jsTestLog(explain);
    winningPlan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, winningPlan));
}

{
    jsTestLog(
        "Testing regex filters with not simple collation on index with not simple collation too...");
    assert.commandWorked(collection.dropIndex({value: 1}));
    assert.commandWorked(collection.createIndex({value: 1}, {collation: {locale: "fr"}}));

    jsTestLog("Testing that prefix regexes use collscan");

    let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1})
                      .collation({locale: "fr"})
                      .toArray();
    assert.sameMembers([{value: "c"}], results);

    let explain = collection.find({value: {$regex: "^c"}}).collation({locale: "fr"}).explain();
    jsTestLog(explain);
    let winningPlan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, winningPlan));

    jsTestLog("Testing that non prefix regexes use collscan");

    results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1})
                  .collation({locale: "fr"})
                  .toArray();
    assert.sameMembers([{value: "c"}], results);

    explain = collection.find({value: {$regex: "c"}}).collation({locale: "fr"}).explain();
    jsTestLog(explain);
    winningPlan = getWinningPlanFromExplain(explain);
    assert(isCollscan(db, winningPlan));
}
