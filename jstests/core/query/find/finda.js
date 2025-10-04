// @tags: [
//   requires_getmore,
//   # This test relies on query commands returning specific batch-sized responses.
//   assumes_no_implicit_cursor_exhaustion,
// ]

// Tests where the QueryOptimizerCursor enters takeover mode during a query rather than a get more.

let t = db.jstests_finda;
t.drop();

const findCommandBatchSize = assert.commandWorked(
    db.adminCommand({getParameter: 1, internalQueryFindCommandBatchSize: 1}),
)["internalQueryFindCommandBatchSize"];
let numDocs = 200;
const maxResultSize = Math.min(findCommandBatchSize, numDocs);

function clearQueryPlanCache() {
    t.createIndex({c: 1});
    t.dropIndex({c: 1});
}

function assertAllFound(matches) {
    //    printjson( matches );
    let found = new Array(numDocs);
    for (var i = 0; i < numDocs; ++i) {
        found[i] = false;
    }
    for (var i in matches) {
        let m = matches[i];
        found[m._id] = true;
    }
    for (var i = 0; i < numDocs; ++i) {
        assert(found[i], i.toString());
    }
}

function makeCursor(query, projection, sort, batchSize, returnKey) {
    print("\n*** query:");
    printjson(query);
    print("proj:");
    printjson(projection);
    let cursor = t.find(query, projection);
    if (sort) {
        cursor.sort(sort);
        print("sort:");
        printjson(sort);
    }
    if (batchSize) {
        cursor.batchSize(batchSize);
        print("bs: " + batchSize);
    }
    if (returnKey) {
        cursor.returnKey();
    }
    return cursor;
}

function checkCursorWithBatchSizeProjection(query, projection, sort, batchSize, expectedLeftInBatch) {
    clearQueryPlanCache();
    let cursor = makeCursor(query, projection, sort, batchSize);

    assert.eq(expectedLeftInBatch, cursor.objsLeftInBatch());
    assertAllFound(cursor.toArray());
}

function checkCursorWithBatchSize(query, sort, batchSize, expectedLeftInBatch) {
    checkCursorWithBatchSizeProjection(query, {}, sort, batchSize, expectedLeftInBatch);
    checkCursorWithBatchSizeProjection(query, {a: 1, _id: 1}, sort, batchSize, expectedLeftInBatch);
    // In the cases tested, when expectedLeftInBatch is high enough takeover will occur during
    // the query operation rather than getMore and the last few matches should properly return keys
    // from the a,_id index.
    clearQueryPlanCache();
    if (expectedLeftInBatch > 110) {
        let cursor = makeCursor(query, {}, sort, batchSize, true);
        let lastNonAIndexResult = -1;
        for (let i = 0; i < expectedLeftInBatch; ++i) {
            let next = cursor.next();
            // Identify the query plan used by checking the fields of a returnKey query.
            if (!friendlyEqual(["a", "_id"], Object.keySet(next))) {
                lastNonAIndexResult = i;
            }
        }
        // The last results should come from the a,_id index.
        assert.lt(lastNonAIndexResult, expectedLeftInBatch - 5);
    }
}

function queryWithPlanTypes(withDups) {
    t.drop();
    for (let i = 1; i < numDocs; ++i) {
        t.save({_id: i, a: i, b: 0});
    }
    if (withDups) {
        t.save({_id: 0, a: [0, numDocs], b: 0}); // Add a dup on a:1 index.
    } else {
        t.save({_id: 0, a: 0, b: 0});
    }
    t.createIndex({a: 1, _id: 1}); // Include _id for a covered index projection.

    // All plans in order.
    checkCursorWithBatchSize({a: {$gte: 0}}, null, 150, 150);

    // All plans out of order.
    checkCursorWithBatchSize({a: {$gte: 0}}, {c: 1}, null, maxResultSize);

    // Some plans in order, some out of order.
    checkCursorWithBatchSize({a: {$gte: 0}, b: 0}, {a: 1}, 150, 150);
    checkCursorWithBatchSize({a: {$gte: 0}, b: 0}, {a: 1}, null, maxResultSize);
}

queryWithPlanTypes(false);
queryWithPlanTypes(true);
