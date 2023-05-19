/**
 * Tests resource consumption metrics for index builds.
 * @tags: [
 *   requires_replication,
 * ]
 */
(function() {
'use strict';

load('jstests/noPassthrough/libs/index_build.js');  // For IndexBuildTest

var rst = new ReplSetTest({
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

/**
 * Build an index. Expect that metrics are reasonable and only reported on the primary node.
 */
(function buildIndex() {
    clearMetrics(primary);
    assert.commandWorked(primaryDB[collName].createIndex({a: 1}));

    assertMetrics(primary, (metrics) => {
        // Each document is 29 bytes. Assert that we read at least as many document bytes as there
        // are in the collection. Some additional data is read from the catalog, but it has
        // randomized fields, so we don't make any exact assertions.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 29 * nDocs);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 1 * nDocs);

        // We intentionally do not collect sorting metrics for index builds due to their already
        // high impact on the server.
        assert.eq(metrics[dbName].primaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].primaryMetrics.sorterSpills, 0);

        // Some bytes are written to the catalog and config.system.indexBuilds collection.
        assert.gt(metrics[dbName].docBytesWritten, 0);
        assert.gt(metrics[dbName].docUnitsWritten, 0);

        // The inserted keys will vary in size from 4 to 7 bytes depending on their value. Assert
        // that the number of bytes fall within that range.
        assert.gt(metrics[dbName].idxEntryBytesWritten, 4 * nDocs);
        assert.lt(metrics[dbName].idxEntryBytesWritten, 7 * nDocs);

        // Some index entries are written to the config.system.indexBuilds collection, but we should
        // read at least as many units as there are documents in the collection.
        assert.gte(metrics[dbName].idxEntryUnitsWritten, 1 * nDocs);
    });

    // The secondary should not collect metrics for replicated index builds.
    rst.awaitReplication();
    assertMetrics(secondary, (metrics) => {
        assert.eq(undefined, metrics[dbName]);
    });
})();

assert.commandWorked(primaryDB[collName].dropIndex({a: 1}));

/**
 * Build an index. Expect that metrics are reasonable and only reported on the primary node.
 */
(function buildUniqueIndex() {
    clearMetrics(primary);
    assert.commandWorked(primaryDB[collName].createIndex({a: 1}, {unique: true}));

    assertMetrics(primary, (metrics) => {
        // Each document is 29 bytes. Assert that we read at least as many document bytes as there
        // are in the collection. Some additional data is read from the catalog, but it has
        // randomized fields, so we don't make any exact assertions.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 29 * nDocs);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 1 * nDocs);

        // We intentionally do not collect sorting metrics for index builds due to their already
        // high impact on the server.
        assert.eq(metrics[dbName].primaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].primaryMetrics.sorterSpills, 0);

        // Some bytes are written to the catalog and config.system.indexBuilds collection.
        assert.gt(metrics[dbName].docBytesWritten, 0);
        assert.gt(metrics[dbName].docUnitsWritten, 0);

        // The inserted keys will vary in size from 4 to 7 bytes depending on their value. Assert
        // that the number of bytes fall within that range.
        assert.gt(metrics[dbName].idxEntryBytesWritten, 4 * nDocs);
        assert.lt(metrics[dbName].idxEntryBytesWritten, 7 * nDocs);

        // Some index entries are written to the config.system.indexBuilds collection, but we should
        // read at least as many units as there are documents in the collection.
        assert.gte(metrics[dbName].idxEntryUnitsWritten, 1 * nDocs);
    });

    // The secondary should not collect metrics for replicated index builds.
    rst.awaitReplication();
    assertMetrics(secondary, (metrics) => {
        assert.eq(undefined, metrics[dbName]);
    });
})();

assert.commandWorked(primaryDB[collName].dropIndex({a: 1}));

/**
 * Build a unique index that fails. Expect that metrics are reasonable and only reported on the
 * primary node.
 */
(function buildFailedUniqueIndex() {
    // Insert a document at the end that makes the index non-unique.
    assert.commandWorked(primaryDB[collName].insert({a: (nDocs - 1)}));

    clearMetrics(primary);
    assert.commandFailedWithCode(primaryDB[collName].createIndex({a: 1}, {unique: true}),
                                 ErrorCodes.DuplicateKey);

    assertMetrics(primary, (metrics) => {
        // Each document is 29 bytes. Assert that we read at least as many document bytes as there
        // are in the collection. Some additional data is read from the catalog, but it has
        // randomized fields, so we don't make any exact assertions.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 29 * nDocs);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 1 * nDocs);

        // We intentionally do not collect sorting metrics for index builds due to their already
        // high impact on the server.
        assert.eq(metrics[dbName].primaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].primaryMetrics.sorterSpills, 0);

        // Some bytes are written to the catalog and config.system.indexBuilds collection.
        assert.gt(metrics[dbName].docBytesWritten, 0);
        assert.gt(metrics[dbName].docUnitsWritten, 0);

        // The inserted keys will vary in size from 4 to 7 bytes depending on their value. Assert
        // that the number of bytes fall within that range.
        assert.gt(metrics[dbName].idxEntryBytesWritten, 4 * nDocs);
        assert.lt(metrics[dbName].idxEntryBytesWritten, 7 * nDocs);

        // Some index entries are written to the config.system.indexBuilds collection, but we should
        // read at least as many units as there are documents in the collection.
        assert.gte(metrics[dbName].idxEntryUnitsWritten, 1 * nDocs);
    });

    // The secondary should not collect metrics for replicated index builds.
    rst.awaitReplication();
    assertMetrics(secondary, (metrics) => {
        assert.eq(undefined, metrics[dbName]);
    });
})();

/**
 * Abort an active index build. Expect that the primary node that aborts the index build collects
 * and reports read metrics.
 */
(function buildIndexInterrupt() {
    clearMetrics(primary);

    // Hang the index build after kicking off the build on the primary, but before scanning the
    // collection.
    const failPoint = configureFailPoint(primary, 'hangAfterStartingIndexBuildUnlocked');
    const awaitIndex = IndexBuildTest.startIndexBuild(
        primary, primaryDB[collName].getFullName(), {a: 1}, {}, [ErrorCodes.IndexBuildAborted]);

    // Waits until the collection scan is finished.
    failPoint.wait();

    // Abort the index build and wait for it to exit.
    const abortIndexThread =
        startParallelShell('assert.commandWorked(db.getMongo().getCollection("' +
                               primaryDB[collName].getFullName() + '").dropIndex({a: 1}))',
                           primary.port);
    checkLog.containsJson(primary, 4656010);

    failPoint.off();

    abortIndexThread();
    awaitIndex();

    // Wait for the abort to replicate.
    rst.awaitReplication();

    assertMetrics(primary, (metrics) => {
        printjson(metrics);
        // Each document is 29 bytes. Assert that we read at least as many document bytes as there
        // are in the collection since the index build is interrupted after this step. Some
        // additional data is read from the catalog, but it has randomized fields, so we don't make
        // any exact assertions.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 29 * nDocs);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 1 * nDocs);
        assert.eq(metrics[dbName].secondaryMetrics.docBytesRead, 0);
        assert.eq(metrics[dbName].secondaryMetrics.docUnitsRead, 0);

        // We intentionally do not collect sorting metrics for index builds due to their already
        // high impact on the server.
        assert.eq(metrics[dbName].primaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].primaryMetrics.sorterSpills, 0);
        assert.eq(metrics[dbName].secondaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].secondaryMetrics.sorterSpills, 0);

        // Some bytes are written to the catalog and config.system.indexBuilds collection.
        assert.gt(metrics[dbName].docBytesWritten, 0);
        assert.gt(metrics[dbName].docUnitsWritten, 0);

        // The index build will have been interrupted before inserting any keys into the index,
        // however it will have written documents into the config.system.indexBuilds collection when
        // created and interrupted by the drop.
        assert.gt(metrics[dbName].idxEntryBytesWritten, 0);
        assert.lte(metrics[dbName].idxEntryBytesWritten, 40);
        assert.gt(metrics[dbName].idxEntryUnitsWritten, 0);
        assert.lte(metrics[dbName].idxEntryUnitsWritten, 4);

        assert.lt(metrics[dbName].idxEntryUnitsWritten, 1 * nDocs);
    });

    // No metrics should be collected on the secondary.
    assertMetrics(secondary, (metrics) => {
        assert(!metrics[dbName]);
    });

    // Ensure the index was actually built. Do this after checking metrics because the helper calls
    // listIndexes which contributes to metrics.
    IndexBuildTest.assertIndexes(primaryDB[collName], 1, ['_id_']);
    IndexBuildTest.assertIndexes(secondaryDB[collName], 1, ['_id_']);
})();

/**
 * Start an index build on one node and commit it on a different node. Expect that the primary node
 * that commits the index build collects and reports read metrics attributed to the primary state.
 * The the stepped-down node should not collect anything.
 */
(function buildIndexWithStepDown() {
    clearMetrics(primary);
    clearMetrics(secondary);

    // Hang the index build after kicking off the build on the secondary, but before scanning the
    // collection.
    IndexBuildTest.pauseIndexBuilds(primary);
    const awaitIndex = IndexBuildTest.startIndexBuild(primary,
                                                      primaryDB[collName].getFullName(),
                                                      {a: 1},
                                                      {},
                                                      [ErrorCodes.InterruptedDueToReplStateChange]);
    IndexBuildTest.waitForIndexBuildToStart(secondaryDB);

    // Step down the primary node. The command will return an error but the index build will
    // continue running.
    assert.commandWorked(primary.adminCommand({replSetStepDown: 60, force: true}));
    awaitIndex();

    // Allow the secondary to take over. Note that it needs a quorum (by default a majority) and
    // will wait for the old primary to complete.
    rst.stepUp(secondary);

    // Allow the index build to resume and wait for it to commit.
    IndexBuildTest.resumeIndexBuilds(primary);
    IndexBuildTest.waitForIndexBuildToStop(secondaryDB);
    rst.awaitReplication();

    // Get the metrics from what is now the new primary.
    assertMetrics(secondary, (metrics) => {
        // Each document is 29 bytes. Assert that we read at least as many document bytes as there
        // are in the collection. Some additional data is read from the catalog, but it has
        // randomized fields, so we don't make any exact assertions.
        assert.gt(metrics[dbName].primaryMetrics.docBytesRead, 29 * nDocs);
        assert.gt(metrics[dbName].primaryMetrics.docUnitsRead, 1 * nDocs);
        assert.eq(metrics[dbName].secondaryMetrics.docBytesRead, 0);
        assert.eq(metrics[dbName].secondaryMetrics.docUnitsRead, 0);

        // We intentionally do not collect sorting metrics for index builds due to their already
        // high impact on the server.
        assert.eq(metrics[dbName].primaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].primaryMetrics.sorterSpills, 0);
        assert.eq(metrics[dbName].secondaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].secondaryMetrics.sorterSpills, 0);

        // Some bytes are written to the catalog and config.system.indexBuilds collection.
        assert.gt(metrics[dbName].docBytesWritten, 0);
        assert.gt(metrics[dbName].docUnitsWritten, 0);

        // The inserted keys will vary in size from 4 to 7 bytes depending on their value. Assert
        // that the number of bytes fall within that range.
        assert.gt(metrics[dbName].idxEntryBytesWritten, 4 * nDocs);
        assert.lt(metrics[dbName].idxEntryBytesWritten, 7 * nDocs);

        // Some index entries are written to the config.system.indexBuilds collection, but we should
        // read at least as many units as there are documents in the collection.
        assert.gte(metrics[dbName].idxEntryUnitsWritten, 1 * nDocs);
    });

    // No significant metrics should be collected on the old primary.
    assertMetrics(primary, (metrics) => {
        // The old primary may have read document bytes on the catalog and config.system.indexBuilds
        // when setting up, but ensure that it does not read an entire collection's worth of data.
        // The reads are attributed to the secondary state because the node is no longer primary
        // when it aggregates its metrics after getting interrupted by the stepdown.
        assert.gte(metrics[dbName].primaryMetrics.docBytesRead, 0);
        assert.lt(metrics[dbName].primaryMetrics.docBytesRead, 29 * nDocs);
        assert.lt(metrics[dbName].secondaryMetrics.docBytesRead, 29 * nDocs);

        // We intentionally do not collect sorting metrics for index builds due to their already
        // high impact on the server.
        assert.eq(metrics[dbName].primaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].primaryMetrics.sorterSpills, 0);
        assert.eq(metrics[dbName].secondaryMetrics.keysSorted, 0);
        assert.eq(metrics[dbName].secondaryMetrics.sorterSpills, 0);

        assert.eq(metrics[dbName].docBytesWritten, 0);
        assert.eq(metrics[dbName].docUnitsWritten, 0);
        assert.eq(metrics[dbName].idxEntryBytesWritten, 0);
        assert.eq(metrics[dbName].idxEntryBytesWritten, 0);
        assert.eq(metrics[dbName].idxEntryUnitsWritten, 0);
    });

    // Ensure the index was actually built. Do this after checking metrics because the helper calls
    // listIndexes which contributes to metrics.
    IndexBuildTest.assertIndexes(primaryDB[collName], 2, ['_id_', 'a_1']);
    IndexBuildTest.assertIndexes(secondaryDB[collName], 2, ['_id_', 'a_1']);
})();
rst.stopSet();
}());
