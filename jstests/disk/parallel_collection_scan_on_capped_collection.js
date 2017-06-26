/**
* Tests that calling the 'parallelCollectionScan' command on a capped collection
* always only returns one cursor and that the document insertion order is maintained
* when iterating over that cursor.
*
* This test requires the use of mmapv1 as the storage engine. The 'parallelCollectionScan'
* command is not yet fully supported for wiredTiger and currently will always return only
* one cursor regardless of the type of collection the command is run on.
* @tags: [requires_mmapv1]
*/

(function() {
    'use strict';
    let nonCappedCollName = 'noncapped_coll';
    let cappedCollName = 'capped_coll';

    // Create a non-capped collection.
    assert.commandWorked(db.runCommand({create: nonCappedCollName}));
    // Create a capped collection with the size of 4096 bytes.
    assert.commandWorked(db.runCommand({create: cappedCollName, capped: true, size: 4096}));

    let nonCappedBulk = db[nonCappedCollName].initializeUnorderedBulkOp();
    let cappedBulk = db[cappedCollName].initializeUnorderedBulkOp();

    // Add enough documents to each collection to ensure that more than one extent
    // on disk is populated. The 'parallelCollectionScan' command on non-capped
    // collections returns up to one cursor per extent.
    for (let i = 0; i < 500; i++) {
        nonCappedBulk.insert({key: i});
        cappedBulk.insert({key: i});
    }
    assert.writeOK(nonCappedBulk.execute());
    assert.writeOK(cappedBulk.execute());

    // Tests that calling 'parallelCollectionScan' with 'numCursors'>=1 on a
    // non-capped collection will return multiple cursors.
    let cmd = {parallelCollectionScan: nonCappedCollName, numCursors: 2};
    let res = assert.commandWorked(db.runCommand(cmd), 'Command failed: ' + tojson(cmd));
    assert.eq(res.cursors.length, 2);

    // Tests that calling 'parallelCollectionScan' on a capped collection will return only
    // one cursor for the case where 'numCursors'>=1.
    let maxCursors = 3;
    for (let numCursors = 1; numCursors < maxCursors; numCursors++) {
        cmd = {parallelCollectionScan: cappedCollName, numCursors: numCursors};
        res = assert.commandWorked(db.runCommand(cmd), 'Command failed: ' + tojson(cmd));
        assert.eq(res.cursors.length, 1);
    }

    // Tests that the document return order of 'parallelCollectionScan' on a capped collection
    // is consistent with the document insertion order.
    cmd = {parallelCollectionScan: cappedCollName, numCursors: 1};
    let pcsResult = assert.commandWorked(db.runCommand(cmd), 'Command failed: ' + tojson(cmd));
    assert.eq(pcsResult.cursors.length, 1);
    let pcsCursor = pcsResult.cursors[0].cursor;
    let pcsGetMore = {
        getMore: pcsResult.cursors[0].cursor.id,
        collection: cappedCollName,
        batchSize: 1
    };
    let pcsGetMoreResult =
        assert.commandWorked(db.runCommand(pcsGetMore), 'Command failed: ' + tojson(pcsGetMore));
    // The sequence of values being returned should be monotonically increasing by one until the
    // last batch.
    let initKey = pcsGetMoreResult.cursor.nextBatch[0].key;
    for (let i = initKey; i < (initKey + db[cappedCollName].count()); i++) {
        assert.eq(pcsGetMoreResult.cursor.nextBatch[0].key, i);
        pcsGetMoreResult = assert.commandWorked(db.runCommand(pcsGetMore),
                                                'Command Failed: ' + tojson(pcsGetMore));
    }
}());
