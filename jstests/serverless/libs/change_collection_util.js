// Contains functions for testing the change collections.

// Verifies that the oplog and change collection entries are the same for the specified start and
// end duration of the oplog timestamp.
function verifyChangeCollectionEntries(connection, startOplogTimestamp, endOplogTimestamp) {
    const oplogColl = connection.getDB("local").oplog.rs;
    const changeColl = connection.getDB("config").system.change_collection;

    // Fetch all oplog and change collection entries for the duration: [startOplogTimestamp,
    // endOplogTimestamp].
    const oplogEntries =
        oplogColl.find({$and: [{ts: {$gte: startOplogTimestamp}}, {ts: {$lte: endOplogTimestamp}}]})
            .toArray();
    const changeCollectionEntries =
        changeColl
            .find({$and: [{_id: {$gte: startOplogTimestamp}}, {_id: {$lte: endOplogTimestamp}}]})
            .toArray();

    assert.eq(
        oplogEntries.length,
        changeCollectionEntries.length,
        "Number of entries in the oplog and the change collection is not the same. Oplog has total " +
            oplogEntries.length + " entries , change collection has total " +
            changeCollectionEntries.length + " entries" +
            "change collection entries " + tojson(changeCollectionEntries));

    for (let idx = 0; idx < oplogEntries.length; idx++) {
        const oplogEntry = oplogEntries[idx];
        const changeCollectionEntry = changeCollectionEntries[idx];

        // Remove the '_id' field from the change collection as oplog does not have it.
        assert(changeCollectionEntry.hasOwnProperty("_id"));
        assert.eq(timestampCmp(changeCollectionEntry._id, oplogEntry.ts),
                  0,
                  "Change collection '_id' field: " + tojson(changeCollectionEntry._id) +
                      " is not same as the oplog 'ts' field: " + tojson(oplogEntry.ts));
        delete changeCollectionEntry["_id"];

        // Verify that the oplog and change collecton entry (after removing the '_id') field are
        // the same.
        assert.eq(
            oplogEntry,
            changeCollectionEntry,
            "Oplog and change collection entries are not same. Oplog entry: " + tojson(oplogEntry) +
                ", change collection entry: " + tojson(changeCollectionEntry));
    }
}
