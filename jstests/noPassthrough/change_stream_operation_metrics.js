/**
 * Tests resource consumption metrics aggregate for change streams.
 *
 * @tags: [
 *   requires_replication,
 * ]
 */
const rst = new ReplSetTest({
    nodes: 2,
    nodeOptions: {setParameter: {"aggregateOperationResourceConsumptionMetrics": true}}
});
rst.startSet();
rst.initiate();

const dbName = 'test';
const collName = 'test';
const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);

const nDocs = 100;

const clearMetrics = (conn) => {
    conn.getDB('admin').aggregate([{$operationMetrics: {clearMetrics: true}}]);
};

// Get aggregated metrics keyed by database name.
const getMetrics = (conn) => {
    const cursor = conn.getDB('admin').aggregate([{$operationMetrics: {}}]);

    let allMetrics = {};
    while (cursor.hasNext()) {
        let doc = cursor.next();
        allMetrics[doc.db] = doc;
    }
    return allMetrics;
};

const assertMetrics = (conn, assertFn) => {
    let metrics = getMetrics(conn);
    try {
        assertFn(metrics);
    } catch (e) {
        print("caught exception while checking metrics on " + tojson(conn) +
              ", metrics: " + tojson(metrics));

        // Since change streams read from the oplog, print the last oplog entry to provide some
        // insight into why the test's expectations differed from reality.
        try {
            let cur = conn.getDB('local').oplog.rs.find({}).sort({$natural: -1});
            print('top of oplog: ' + tojson(cur.next()));
        } catch (e2) {
            print('failed to print the top of oplog' + e2);
        }
        throw e;
    }
};

assert.commandWorked(primaryDB.createCollection(collName));

/**
 * Load documents into the collection. Expect that metrics are reasonable and only reported on the
 * primary node.
 */
(function loadCollection() {
    clearMetrics(primary);

    let bulk = primaryDB[collName].initializeUnorderedBulkOp();
    for (let i = 0; i < nDocs; i++) {
        bulk.insert({_id: i, a: i});
    }
    assert.commandWorked(bulk.execute());

    assertMetrics(primary, (metrics) => {
        // Each document is 29 bytes and we do not count oplog writes.
        assert.eq(metrics[dbName].docBytesWritten, 29 * nDocs);
        assert.eq(metrics[dbName].docUnitsWritten, nDocs);

        // With batch inserts, the index updates are all performed together after all the documents
        // are inserted, so this has the effect of associating all the index bytes for the batch
        // with one document, for the purposes of totalUnitsWritten.  This effect causes the last
        // document to have 3 units instead of 1 like the first 99.
        assert.eq(metrics[dbName].totalUnitsWritten, nDocs + 2);

        // The inserted keys will vary in size from 2 to 4 bytes depending on their value. Assert
        // that the number of bytes fall within that range.
        assert.gt(metrics[dbName].idxEntryBytesWritten, 2 * nDocs);
        assert.lt(metrics[dbName].idxEntryBytesWritten, 4 * nDocs);
        assert.eq(metrics[dbName].idxEntryUnitsWritten, 1 * nDocs);
    });

    // The secondary should not collect metrics for replicated index builds.
    rst.awaitReplication();

    assertMetrics(secondary, (metrics) => {
        assert.eq(undefined, metrics[dbName]);
    });
})();

let nextId = nDocs;

(function changeStreamPrimary() {
    clearMetrics(primary);
    const cur = primaryDB[collName].watch([], {fullDocument: "updateLookup"});

    assertMetrics(primary, (metrics) => {
        // The first aggregate operation will read from the top of the oplog, size not guaranteed.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 0);
        assert.gt(metrics[dbName].primaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsReturned, 0);
    });

    // Ensure that while nothing is returned from the change stream, the server still measures read
    // activity.
    clearMetrics(primary);
    assert(!cur.hasNext());
    assertMetrics(primary, (metrics) => {
        // Calling hasNext may perform many reads from the oplog. The oplog entry size is not
        // guaranteed.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 0);
        assert.gt(metrics[dbName].primaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsReturned, 0);
    });

    // Insert a document and ensure its metrics are aggregated.
    clearMetrics(primary);
    const doc = {_id: nextId, a: nextId};
    nextId += 1;
    assert.commandWorked(primaryDB[collName].insert(doc));
    assertMetrics(primary, (metrics) => {
        assert.eq(metrics[dbName].docBytesWritten, 29);
        assert.eq(metrics[dbName].docUnitsWritten, 1);
        assert.eq(metrics[dbName].idxEntryBytesWritten, 3);
        assert.eq(metrics[dbName].idxEntryUnitsWritten, 1);
        assert.eq(metrics[dbName].totalUnitsWritten, 1);
        assert.eq(metrics[dbName].primaryMetrics.docBytesRead, 0);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsRead, 0);
        assert.eq(metrics[dbName].primaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsReturned, 0);
    });

    clearMetrics(primary);

    // Ensure that the inserted document eventually comes through the change stream.
    assert.soon(() => {
        if (cur.hasNext()) {
            return true;
        }
        print("Change stream returned no data. Clearing metrics and retrying.");
        clearMetrics(primary);
        return false;
    });
    assert.eq(doc, cur.next().fullDocument);
    assertMetrics(primary, (metrics) => {
        // Will read at least one document from the oplog.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 0);
        assert.gt(metrics[dbName].primaryMetrics.cursorSeeks, 0);
        // Returns one large document
        assert.eq(metrics[dbName].primaryMetrics.docUnitsReturned, 4);
    });

    // Update the document and ensure the metrics are aggregated.
    clearMetrics(primary);
    assert.commandWorked(primaryDB[collName].update(doc, {$set: {b: 0}}));
    assertMetrics(primary, (metrics) => {
        assert.eq(metrics[dbName].docBytesWritten, 11);
        assert.eq(metrics[dbName].docUnitsWritten, 1);
        assert.eq(metrics[dbName].totalUnitsWritten, 1);
        assert.eq(metrics[dbName].primaryMetrics.docBytesRead, 29);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsRead, 1);
        assert.eq(metrics[dbName].primaryMetrics.idxEntryBytesRead, 3);
        assert.eq(metrics[dbName].primaryMetrics.idxEntryUnitsRead, 1);
        assert.gt(metrics[dbName].primaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsReturned, 0);
    });

    clearMetrics(primary);

    // Ensure that the updated document eventually comes through the change stream.
    assert.soon(() => {
        if (cur.hasNext()) {
            return true;
        }
        print("Change stream returned no data. Clearing metrics and retrying.");
        clearMetrics(primary);
        return false;
    });
    const newDoc = Object.assign({b: 0}, doc);
    let res = cur.next();
    assert.docEq(newDoc, res.fullDocument, res);
    assertMetrics(primary, (metrics) => {
        // Performs at least three seeks (oplog, _id index, collection), reads at least one entry
        // from the oplog, once from the collection, and then returns one large response document.
        assert.gte(metrics[dbName].primaryMetrics.docBytesRead, 0);
        assert.gte(metrics[dbName].primaryMetrics.docUnitsRead, 2);
        assert.eq(metrics[dbName].primaryMetrics.idxEntryBytesRead, 3);
        assert.eq(metrics[dbName].primaryMetrics.idxEntryUnitsRead, 1);
        assert.gt(metrics[dbName].primaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].primaryMetrics.docUnitsReturned, 4);
    });
})();

(function changeStreamSecondary() {
    clearMetrics(secondary);
    const cur = secondaryDB[collName].watch([], {fullDocument: "updateLookup"});

    assertMetrics(secondary, (metrics) => {
        // The first aggregate operation will read one document from the oplog, size not guaranteed.
        assert.gt(metrics[dbName].secondaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].secondaryMetrics.docUnitsRead, 0);
        assert.gt(metrics[dbName].secondaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].secondaryMetrics.docUnitsReturned, 0);
    });

    // Ensure that while nothing is returned from the change stream, the server still measures read
    // activity.
    clearMetrics(secondary);
    assert(!cur.hasNext());
    assertMetrics(secondary, (metrics) => {
        // Calling hasNext may perform many reads from the oplog, and the size is not guaranteed.
        assert.gt(metrics[dbName].secondaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].secondaryMetrics.docUnitsRead, 0);
        assert.gt(metrics[dbName].secondaryMetrics.cursorSeeks, 0);
        assert.eq(metrics[dbName].secondaryMetrics.docUnitsReturned, 0);
    });

    // Insert a document and ensure the secondary collects no metrics.
    clearMetrics(secondary);
    const doc = {_id: nextId, a: nextId};
    assert.commandWorked(primaryDB[collName].insert(doc));
    rst.awaitReplication();
    assertMetrics(secondary, (metrics) => {
        assert(!metrics[dbName]);
    });

    // Ensure that the inserted document eventually comes through the change stream.
    assert.soon(() => {
        if (cur.hasNext()) {
            return true;
        }
        print("Change stream returned no data. Clearing metrics and retrying.");
        clearMetrics(secondary);
        return false;
    });
    assert.eq(doc, cur.next().fullDocument);
    assertMetrics(secondary, (metrics) => {
        // Performs one seek on the oplog, read at least one entry, and then returns one large
        // response document.
        assert.gt(metrics[dbName].secondaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].secondaryMetrics.docUnitsRead, 0);
        assert.gte(metrics[dbName].secondaryMetrics.cursorSeeks, 1);
        assert.gte(metrics[dbName].secondaryMetrics.docUnitsReturned, 3);
    });

    // Update the document and ensure the secondary collects no metrics.
    clearMetrics(secondary);
    assert.commandWorked(primaryDB[collName].update(doc, {$set: {b: 0}}));
    rst.awaitReplication();
    assertMetrics(secondary, (metrics) => {
        assert(!metrics[dbName]);
    });

    // Ensure that the updated document eventually comes through the change stream.
    assert.soon(() => {
        if (cur.hasNext()) {
            return true;
        }
        print("Change stream returned no data. Clearing metrics and retrying.");
        clearMetrics(secondary);
        return false;
    });
    const newDoc = Object.assign({b: 0}, doc);
    let res = cur.next();
    assert.docEq(newDoc, res.fullDocument, res);
    assertMetrics(secondary, (metrics) => {
        // Performs at least three seeks (oplog, _id index, collection), reads at least one entry
        // from the oplog and once from the collection, and then returns one large response
        // document.
        assert.gt(metrics[dbName].secondaryMetrics.docBytesRead, 0);
        assert.gt(metrics[dbName].secondaryMetrics.docUnitsRead, 0);
        assert.eq(metrics[dbName].secondaryMetrics.idxEntryBytesRead, 3);
        assert.eq(metrics[dbName].secondaryMetrics.idxEntryUnitsRead, 1);
        assert.gte(metrics[dbName].secondaryMetrics.cursorSeeks, 3);
        assert.gte(metrics[dbName].secondaryMetrics.docUnitsReturned, 4);
    });
})();
rst.stopSet();
