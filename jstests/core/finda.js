// Tests where the QueryOptimizerCursor enters takeover mode during a query rather than a get more.

t = db.jstests_finda;
t.drop();

numDocs = 200;

function clearQueryPlanCache() {
    t.ensureIndex({c: 1});
    t.dropIndex({c: 1});
}

function assertAllFound(matches) {
    //    printjson( matches );
    found = new Array(numDocs);
    for (var i = 0; i < numDocs; ++i) {
        found[i] = false;
    }
    for (var i in matches) {
        m = matches[i];
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
    cursor = t.find(query, projection);
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

function checkCursorWithBatchSizeProjection(
    query, projection, sort, batchSize, expectedLeftInBatch) {
    clearQueryPlanCache();
    cursor = makeCursor(query, projection, sort, batchSize);
    // XXX: this
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
        cursor = makeCursor(query, {}, sort, batchSize, true);
        lastNonAIndexResult = -1;
        for (var i = 0; i < expectedLeftInBatch; ++i) {
            next = cursor.next();
            // Identify the query plan used by checking the fields of a returnKey query.
            if (!friendlyEqual(['a', '_id'], Object.keySet(next))) {
                lastNonAIndexResult = i;
            }
        }
        // The last results should come from the a,_id index.
        assert.lt(lastNonAIndexResult, expectedLeftInBatch - 5);
    }
}

function queryWithPlanTypes(withDups) {
    t.drop();
    for (var i = 1; i < numDocs; ++i) {
        t.save({_id: i, a: i, b: 0});
    }
    if (withDups) {
        t.save({_id: 0, a: [0, numDocs], b: 0});  // Add a dup on a:1 index.
    } else {
        t.save({_id: 0, a: 0, b: 0});
    }
    t.ensureIndex({a: 1, _id: 1});  // Include _id for a covered index projection.

    // All plans in order.
    checkCursorWithBatchSize({a: {$gte: 0}}, null, 150, 150);

    // All plans out of order.
    checkCursorWithBatchSize({a: {$gte: 0}}, {c: 1}, null, 101);

    // Some plans in order, some out of order.
    checkCursorWithBatchSize({a: {$gte: 0}, b: 0}, {a: 1}, 150, 150);
    checkCursorWithBatchSize({a: {$gte: 0}, b: 0}, {a: 1}, null, 101);
}

queryWithPlanTypes(false);
queryWithPlanTypes(true);
