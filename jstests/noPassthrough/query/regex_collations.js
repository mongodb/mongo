/**
 * Tests how regex filters are satisfied with different index + query collation combinations
 */

import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {
    getPlanStage,
    getWinningPlanFromExplain,
    isCollscan,
    isIxscan
} from "jstests/libs/query/analyze_plan.js";

const conn = MongoRunner.runMongod();
const db = conn.getDB(jsTestName());

function assertIXScanTightBounds(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isIxscan(db, winningPlan), explain);
    const ixscan = getPlanStage(winningPlan, 'IXSCAN');
    assert.doesNotContain("[\"\", {})",
                          ixscan.indexBounds.value,
                          `Unexpected full IXSCAN plan!\n${JSON.stringify(explain, null, 2)}`);
}

function assertFullIXScan(explain) {
    const winningPlan = getWinningPlanFromExplain(explain);
    assert(isIxscan(db, winningPlan), explain);
    const ixscan = getPlanStage(winningPlan, 'IXSCAN');
    assert(ixscan.indexBounds.value[0] == "[\"\", {})" ||
               ixscan.indexBounds.value[0] == "[CollationKey(0x), {})",
           `Expected full IXSCAN plan!\n${JSON.stringify(explain, null, 2)}`);
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
    assertIXScanTightBounds(explain);

    jsTestLog("Testing that non prefix regexes use full index scan plan");

    results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1}).toArray();
    assert.sameMembers([{value: "c"}], results);

    explain = collection.find({value: {$regex: "c"}}).explain();
    assertFullIXScan(explain);
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
    assertIXScanTightBounds(explain);

    jsTestLog("Testing that non prefix regexes use full index scan plan");

    results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1})
                  .collation({locale: "fr"})
                  .toArray();
    assert.sameMembers([{value: "c"}], results);

    explain = collection.find({value: {$regex: "c"}}).collation({locale: "fr"}).explain();
    assertFullIXScan(explain);
}

{
    jsTestLog("Testing regex filters with simple collation on index with not simple collation...");
    assert.commandWorked(collection.dropIndex({value: 1}));
    assert.commandWorked(collection.createIndex({value: 1}, {collation: {locale: "fr"}}));

    {
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQueryPlannerIgnoreIndexWithCollationForRegex: 0,
        }));

        jsTestLog("Testing that prefix regexes use full index scan plan");

        let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1}).toArray();
        assert.sameMembers([{value: "c"}], results);

        let explain = collection.find({value: {$regex: "^c"}}).explain();
        assertFullIXScan(explain);

        jsTestLog("Testing that non prefix regexes use full index scan plan");

        results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1}).toArray();
        assert.sameMembers([{value: "c"}], results);

        explain = collection.find({value: {$regex: "c"}}).explain();
        assertFullIXScan(explain);
    }
    {
        jsTestLog("Ignoring now the index with collation for regexes...");

        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQueryPlannerIgnoreIndexWithCollationForRegex: 1,
        }));

        jsTestLog("Testing that prefix regexes use collscan");

        let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1}).toArray();
        assert.sameMembers([{value: "c"}], results);

        let explain = collection.find({value: {$regex: "^c"}}).explain();
        let winningPlan = getWinningPlanFromExplain(explain);
        assert(isCollscan(db, winningPlan),
               `Expected COLLSCAN plan\n${JSON.stringify(explain, null, 2)}`);

        jsTestLog("Testing that non prefix regexes use collscan");

        results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1}).toArray();
        assert.sameMembers([{value: "c"}], results);

        explain = collection.find({value: {$regex: "c"}}).explain();
        winningPlan = getWinningPlanFromExplain(explain);
        assert(isCollscan(db, winningPlan),
               `Expected COLLSCAN plan\n${JSON.stringify(explain, null, 2)}`);
    }
}

{
    jsTestLog(
        "Testing regex filters with not simple collation on index with not simple collation too...");
    assert.commandWorked(collection.dropIndex({value: 1}));
    assert.commandWorked(collection.createIndex({value: 1}, {collation: {locale: "fr"}}));

    {
        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQueryPlannerIgnoreIndexWithCollationForRegex: 0,
        }));

        jsTestLog("Testing that prefix regexes use full index scan plan");

        let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1})
                          .collation({locale: "fr"})
                          .toArray();
        assert.sameMembers([{value: "c"}], results);

        let explain = collection.find({value: {$regex: "^c"}}).collation({locale: "fr"}).explain();
        assertFullIXScan(explain);

        jsTestLog("Testing that non prefix regexes use full index scan plan");

        results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1})
                      .collation({locale: "fr"})
                      .toArray();
        assert.sameMembers([{value: "c"}], results);

        explain = collection.find({value: {$regex: "c"}}).collation({locale: "fr"}).explain();
        assertFullIXScan(explain);
    }

    {
        jsTestLog("Ignoring now the index with collation for regexes...");

        assert.commandWorked(db.adminCommand({
            setParameter: 1,
            internalQueryPlannerIgnoreIndexWithCollationForRegex: 1,
        }));

        jsTestLog("Testing that prefix regexes use collscan");

        let results = collection.find({value: {$regex: "^c"}}, {_id: 0, value: 1})
                          .collation({locale: "fr"})
                          .toArray();
        assert.sameMembers([{value: "c"}], results);

        let explain = collection.find({value: {$regex: "^c"}}).collation({locale: "fr"}).explain();
        let winningPlan = getWinningPlanFromExplain(explain);
        assert(isCollscan(db, winningPlan),
               `Expected COLLSCAN plan\n${JSON.stringify(explain, null, 2)}`);

        jsTestLog("Testing that non prefix regexes use collscan");

        results = collection.find({value: {$regex: "c"}}, {_id: 0, value: 1})
                      .collation({locale: "fr"})
                      .toArray();
        assert.sameMembers([{value: "c"}], results);

        explain = collection.find({value: {$regex: "c"}}).collation({locale: "fr"}).explain();
        winningPlan = getWinningPlanFromExplain(explain);
        assert(isCollscan(db, winningPlan),
               `Expected COLLSCAN plan\n${JSON.stringify(explain, null, 2)}`);
    }
}

MongoRunner.stopMongod(conn);
