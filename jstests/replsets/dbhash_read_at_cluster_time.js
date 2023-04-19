/**
 * Tests that "$_internalReadAtClusterTime" and "snapshot" level
 * read concern are supported by the "dbHash" command.
 *
 * @tags: [
 *   requires_majority_read_concern,
 *   uses_transactions,
 * ]
 */
(function() {
"use strict";

load("jstests/libs/feature_flag_util.js");

const rst = new ReplSetTest({nodes: 2});
rst.startSet();

const replSetConfig = rst.getReplSetConfig();
replSetConfig.members[1].priority = 0;
rst.initiate(replSetConfig);

const primary = rst.getPrimary();
const secondary = rst.getSecondary();

const session = primary.startSession({causalConsistency: false});
const db = session.getDatabase("test");

// We prevent the replica set from advancing oldest_timestamp. This ensures that the snapshot
// associated with 'clusterTime' is retained for the duration of this test.
rst.nodes.forEach(conn => {
    assert.commandWorked(conn.adminCommand({
        configureFailPoint: "WTPreserveSnapshotHistoryIndefinitely",
        mode: "alwaysOn",
    }));
});

// We insert a document and save the md5sum associated with the opTime of that write.
assert.commandWorked(db.mycoll.insert({_id: 1}, {writeConcern: {w: "majority"}}));
const clusterTime = db.getSession().getOperationTime();

let res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    $_internalReadAtClusterTime: clusterTime,
}));

const internalAtClusterTimeHashBefore = {
    collections: res.collections,
    md5: res.md5
};

res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    readConcern: {level: "snapshot", atClusterTime: clusterTime},
}));

const atClusterTimeHashBefore = {
    collections: res.collections,
    md5: res.md5
};

// We insert another document to ensure the collection's contents have a different md5sum now.
// We use a w=majority write concern to ensure that the insert has also been applied on the
// secondary by the time we go to run the dbHash command later. This avoids a race where the
// replication subsystem could be applying the insert operation when the dbHash command is run
// on the secondary.
assert.commandWorked(db.mycoll.insert({_id: 2}, {writeConcern: {w: "majority"}}));

// However, using $_internalReadAtClusterTime or snapshot read concern to read at the opTime of the
// first insert should return the same md5sum as it did originally.
res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    $_internalReadAtClusterTime: clusterTime,
}));

const internalAtClusterTimeHashAfter = {
    collections: res.collections,
    md5: res.md5
};

res = assert.commandWorked(db.runCommand({
    dbHash: 1,
    readConcern: {level: "snapshot", atClusterTime: clusterTime},
}));

const atClusterTimeHashAfter = {
    collections: res.collections,
    md5: res.md5
};

assert.eq(internalAtClusterTimeHashBefore,
          internalAtClusterTimeHashAfter,
          "primary returned different dbhash after " +
              "second insert while using $_internalReadAtClusterTime");
assert.eq(atClusterTimeHashBefore,
          atClusterTimeHashAfter,
          "primary returned different dbhash after " +
              "second insert while using \"snapshot\" level read concern");

{
    const secondarySession = secondary.startSession({causalConsistency: false});
    const secondaryDB = secondarySession.getDatabase("test");

    // Using $_internalReadAtClusterTime or snapshot read concern to read at the opTime
    // of the first insert should return the same md5sum on the secondary as it did on the primary.
    res = assert.commandWorked(secondaryDB.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: clusterTime,
    }));

    const internalAtClusterTimeSecondaryHash = {collections: res.collections, md5: res.md5};

    res = assert.commandWorked(secondaryDB.runCommand({
        dbHash: 1,
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
    }));

    const atClusterTimeSecondaryHash = {collections: res.collections, md5: res.md5};

    assert.eq(internalAtClusterTimeHashBefore,
              internalAtClusterTimeSecondaryHash,
              "primary and secondary have different dbhash " +
                  "while using $_internalReadAtClusterTime.");
    assert.eq(atClusterTimeHashBefore,
              atClusterTimeSecondaryHash,
              "primary returned different dbhash " +
                  "while using \"snapshot\" level read concern");
}

{
    const otherSession = primary.startSession({causalConsistency: false});
    const otherDB = otherSession.getDatabase("test");

    // We perform another insert inside a separate transaction to cause a MODE_IX lock to be
    // held on the collection.
    otherSession.startTransaction();
    assert.commandWorked(otherDB.mycoll.insert({_id: 3}));

    // It should be possible to run the "dbHash" command with "$_internalReadAtClusterTime"
    // or snapshot read concern concurrently.
    res = assert.commandWorked(db.runCommand({
        dbHash: 1,
        $_internalReadAtClusterTime: clusterTime,
    }));

    const concurrentInternalAtClusterTimeHash = {collections: res.collections, md5: res.md5};

    res = assert.commandWorked(db.runCommand({
        dbHash: 1,
        readConcern: {level: "snapshot", atClusterTime: clusterTime},
    }));

    const concurrentAtClusterTimeHash = {collections: res.collections, md5: res.md5};

    assert.eq(internalAtClusterTimeHashBefore,
              concurrentInternalAtClusterTimeHash,
              "primary returned different dbhash after " +
                  "third insert while using $_internalReadAtClusterTime.");
    assert.eq(atClusterTimeHashBefore,
              concurrentAtClusterTimeHash,
              "primary returned different dbhash after " +
                  "third insert while using \"snapshot\" level read concern");

    // However, the "dbHash" command should block behind the transaction if
    // "$_internalReadAtClusterTime" wasn't specified.
    assert.commandFailedWithCode(db.runCommand({dbHash: 1, maxTimeMS: 1000}),
                                 ErrorCodes.MaxTimeMSExpired);

    assert.commandWorked(otherSession.abortTransaction_forTesting());
    otherSession.endSession();
}

{
    const otherSession = primary.startSession({causalConsistency: false});
    const otherDB = otherSession.getDatabase("test");

    // We create another collection inside a separate session to modify the collection catalog
    // at an opTime later than 'clusterTime'.
    assert.commandWorked(otherDB.runCommand({create: "mycoll2"}));
    assert.commandWorked(db.runCommand({dbHash: 1, $_internalReadAtClusterTime: clusterTime}));

    otherSession.endSession();
}

session.endSession();
rst.stopSet();
})();
