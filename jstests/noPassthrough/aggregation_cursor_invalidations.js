/**
 * Tests that using an aggregation cursor when the underlying PlanExecutor has been killed results
 * in an error. Also tests that if the pipeline has already read all results from a collection
 * before the collection is dropped, the aggregation should succeed.
 *
 * This test issues getMores on aggregation cursors and expects the getMore to cause the aggregation
 * to request more documents from the collection. If the pipeline is wrapped in a $facet stage, all
 * results will be computed in the initial request and buffered in the results array, preventing the
 * pipeline from requesting more documents.
 * @tags: [do_not_wrap_aggregations_in_facets]
 */
(function() {
    'use strict';

    // The DocumentSourceCursor which wraps PlanExecutors will batch results internally. We use the
    // 'internalDocumentSourceCursorBatchSizeBytes' parameter to disable this behavior so that we
    // can easily pause a pipeline in a state where it will need to request more results from the
    // PlanExecutor.
    const options = {setParameter: 'internalDocumentSourceCursorBatchSizeBytes=1'};
    const conn = MongoRunner.runMongod(options);
    assert.neq(null, conn, 'mongod was unable to start up with options: ' + tojson(options));

    const testDB = conn.getDB('test');

    // Make sure the number of results is greater than the batchSize to ensure the results
    // cannot all fit in one batch.
    const batchSize = 2;
    const numMatches = batchSize + 1;
    const sourceCollection = testDB.source;
    const foreignCollection = testDB.foreign;

    /**
     * Populates both 'sourceCollection' and 'foreignCollection' with values of 'local' and
     * 'foreign' in the range [0, 'numMatches').
     */
    function setup() {
        sourceCollection.drop();
        foreignCollection.drop();
        for (let i = 0; i < numMatches; ++i) {
            assert.writeOK(sourceCollection.insert({_id: i, local: i}));

            // We want to be able to pause a $lookup stage in a state where it has returned some but
            // not all of the results for a single lookup, so we need to insert at least
            // 'numMatches' matches for each source document.
            for (let j = 0; j < numMatches; ++j) {
                assert.writeOK(foreignCollection.insert({_id: numMatches * i + j, foreign: i}));
            }
        }
    }

    const defaultAggregateCmdSmallBatch = {
        aggregate: sourceCollection.getName(),
        pipeline: [],
        cursor: {
            batchSize: batchSize,
        },
    };

    // Test that dropping the source collection between an aggregate and a getMore will cause an
    // aggregation pipeline to fail during the getMore if it needs to fetch more results from the
    // collection.
    setup();
    let res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));

    sourceCollection.drop();

    let getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.QueryPlanKilled,
        'expected getMore to fail because the source collection was dropped');

    // Make sure the cursors were cleaned up.
    let serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    // Test that dropping the source collection between an aggregate and a getMore will *not* cause
    // an aggregation pipeline to fail during the getMore if it *does not need* to fetch more
    // results from the collection.
    setup();
    res = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollection.getName(),
        pipeline: [{$sort: {x: 1}}],
        cursor: {
            batchSize: batchSize,
        },
    }));

    sourceCollection.drop();

    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    assert.commandWorked(testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}));

    // Test that dropping a $lookup stage's foreign collection between an aggregate and a getMore
    // will *not* cause an aggregation pipeline to fail during the getMore if it needs to fetch more
    // results from the foreign collection. It will instead return no matches for subsequent
    // lookups, as if the foreign collection was empty.
    setup();
    res = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollection.getName(),
        pipeline: [
            {
              $lookup: {
                  from: foreignCollection.getName(),
                  localField: 'local',
                  foreignField: 'foreign',
                  as: 'results',
              }
            },
        ],
        cursor: {
            batchSize: batchSize,
        },
    }));

    foreignCollection.drop();
    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    res = testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName});
    assert.commandWorked(
        res, 'expected getMore to succeed despite the foreign collection being dropped');
    res.cursor.nextBatch.forEach(function(aggResult) {
        assert.eq(aggResult.results,
                  [],
                  'expected results of $lookup into non-existent collection to be empty');
    });

    // Make sure the cursors were cleaned up.
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    // Test that a $lookup stage will properly clean up its cursor if it becomes invalidated between
    // batches of a single lookup. This is the same scenario as above, but with the $lookup stage
    // left in a state where it has returned some but not all of the matches for a single lookup.
    setup();
    res = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollection.getName(),
        pipeline: [
            {
              $lookup: {
                  from: foreignCollection.getName(),
                  localField: 'local',
                  foreignField: 'foreign',
                  as: 'results',
              }
            },
            // Use an $unwind stage to allow the $lookup stage to return some but not all of the
            // results for a single lookup.
            {$unwind: '$results'},
        ],
        cursor: {
            batchSize: batchSize,
        },
    }));

    foreignCollection.drop();
    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.QueryPlanKilled,
        'expected getMore to fail because the foreign collection was dropped');

    // Make sure the cursors were cleaned up.
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    // Test that dropping a $graphLookup stage's foreign collection between an aggregate and a
    // getMore will *not* cause an aggregation pipeline to fail during the getMore if it needs to
    // fetch more results from the foreign collection. It will instead return no matches for
    // subsequent lookups, as if the foreign collection was empty.
    setup();
    res = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollection.getName(),
        pipeline: [
            {
              $graphLookup: {
                  from: foreignCollection.getName(),
                  startWith: '$local',
                  connectFromField: '_id',
                  connectToField: 'foreign',
                  as: 'results',
              }
            },
        ],
        cursor: {
            batchSize: batchSize,
        },
    }));

    foreignCollection.drop();
    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    res = testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName});
    assert.commandWorked(
        res, 'expected getMore to succeed despite the foreign collection being dropped');

    // Make sure the cursors were cleaned up.
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    // Test that the getMore still succeeds if the $graphLookup is followed by an $unwind on the
    // 'as' field and the collection is dropped between the initial request and a getMore.
    setup();
    res = assert.commandWorked(testDB.runCommand({
        aggregate: sourceCollection.getName(),
        pipeline: [
            {
              $graphLookup: {
                  from: foreignCollection.getName(),
                  startWith: '$local',
                  connectFromField: '_id',
                  connectToField: 'foreign',
                  as: 'results',
              }
            },
            {$unwind: '$results'},
        ],
        cursor: {
            batchSize: batchSize,
        },
    }));

    foreignCollection.drop();
    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    res = testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName});
    assert.commandWorked(
        res, 'expected getMore to succeed despite the foreign collection being dropped');

    // Make sure the cursors were cleaned up.
    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    // Test that dropping the database will kill an aggregation's cursor, causing a subsequent
    // getMore to fail.
    setup();
    res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));

    assert.commandWorked(sourceCollection.getDB().dropDatabase());
    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);

    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.QueryPlanKilled,
        'expected getMore to fail because the database was dropped');

    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    // Test that killing an aggregation's cursor by inserting enough documents to force a truncation
    // of a capped collection will cause a subsequent getMore to fail.
    sourceCollection.drop();
    foreignCollection.drop();
    const maxCappedSizeBytes = 64 * 1024;
    const maxNumDocs = 10;
    assert.commandWorked(testDB.runCommand({
        create: sourceCollection.getName(),
        capped: true,
        size: maxCappedSizeBytes,
        max: maxNumDocs
    }));
    // Fill up about half of the collection.
    for (let i = 0; i < maxNumDocs / 2; ++i) {
        assert.writeOK(sourceCollection.insert({_id: i}));
    }
    // Start an aggregation.
    assert.gt(maxNumDocs / 2, batchSize);
    res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));
    // Insert enough to force a truncation.
    for (let i = maxNumDocs / 2; i < 2 * maxNumDocs; ++i) {
        assert.writeOK(sourceCollection.insert({_id: i}));
    }
    assert.eq(maxNumDocs, sourceCollection.count());
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.QueryPlanKilled,
        'expected getMore to fail because the capped collection was truncated');

    // Test that killing an aggregation's cursor via the killCursors command will cause a subsequent
    // getMore to fail.
    setup();
    res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));

    const killCursorsNamespace = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    assert.commandWorked(
        testDB.runCommand({killCursors: killCursorsNamespace, cursors: [res.cursor.id]}));

    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.CursorNotFound,
        'expected getMore to fail because the cursor was killed');

    // Test that killing an aggregation's operation via the killOp command will cause a getMore to
    // fail.
    setup();
    res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));

    // Use a failpoint to cause a getMore to hang indefinitely.
    assert.commandWorked(testDB.adminCommand(
        {configureFailPoint: 'keepCursorPinnedDuringGetMore', mode: 'alwaysOn'}));
    const curOpFilter = {'query.getMore': res.cursor.id};
    assert.eq(0, testDB.currentOp(curOpFilter).inprog.length);

    getMoreCollName = res.cursor.ns.substr(res.cursor.ns.indexOf('.') + 1);
    const parallelShellCode = 'assert.commandFailedWithCode(db.getSiblingDB(\'' + testDB.getName() +
        '\').runCommand({getMore: ' + res.cursor.id.toString() + ', collection: \'' +
        getMoreCollName +
        '\'}), ErrorCodes.Interrupted, \'expected getMore command to be interrupted by killOp\');';

    // Start a getMore and wait for it to hang.
    const awaitParallelShell = startParallelShell(parallelShellCode, conn.port);
    assert.soon(function() {
        return assert.commandWorked(testDB.currentOp(curOpFilter)).inprog.length === 1;
    }, 'expected getMore operation to remain active');

    // Kill the operation.
    const opId = assert.commandWorked(testDB.currentOp(curOpFilter)).inprog[0].opid;
    assert.commandWorked(testDB.killOp(opId));
    assert.commandWorked(
        testDB.adminCommand({configureFailPoint: 'keepCursorPinnedDuringGetMore', mode: 'off'}));
    assert.eq(0, awaitParallelShell());

    serverStatus = assert.commandWorked(testDB.adminCommand({serverStatus: 1}));
    assert.eq(0,
              serverStatus.metrics.cursor.open.total,
              'expected to find no open cursors: ' + tojson(serverStatus.metrics.cursor));

    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.CursorNotFound,
        'expected getMore to fail because the cursor was killed');

    // Test that a cursor timeout of an aggregation's cursor will cause a subsequent getMore to
    // fail.
    setup();
    res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));

    serverStatus = assert.commandWorked(testDB.serverStatus());
    const expectedNumTimedOutCursors = serverStatus.metrics.cursor.timedOut + 1;

    // Wait until the idle cursor background job has killed the aggregation cursor.
    assert.commandWorked(testDB.adminCommand({setParameter: 1, cursorTimeoutMillis: 10}));
    const cursorTimeoutFrequencySeconds = 1;
    assert.commandWorked(testDB.adminCommand(
        {setParameter: 1, clientCursorMonitorFrequencySecs: cursorTimeoutFrequencySeconds}));
    assert.soon(
        function() {
            serverStatus = assert.commandWorked(testDB.serverStatus());
            return serverStatus.metrics.cursor.timedOut == expectedNumTimedOutCursors;
        },
        function() {
            return 'aggregation cursor failed to time out, expected ' + expectedNumTimedOutCursors +
                ' timed out cursors: ' + tojson(serverStatus.metrics.cursor);
        });

    assert.eq(0, serverStatus.metrics.cursor.open.total, tojson(serverStatus));
    assert.commandFailedWithCode(
        testDB.runCommand({getMore: res.cursor.id, collection: getMoreCollName}),
        ErrorCodes.CursorNotFound,
        'expected getMore to fail because the cursor was killed');

    // Test that a cursor will properly be cleaned up on server shutdown.
    setup();
    res = assert.commandWorked(testDB.runCommand(defaultAggregateCmdSmallBatch));
    assert.eq(0, MongoRunner.stopMongod(conn), 'expected mongod to shutdown cleanly');
})();
