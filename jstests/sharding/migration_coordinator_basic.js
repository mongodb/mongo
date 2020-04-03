/**
 * Tests that a donor shard durably records a migration's state, inserts pending entries into its
 * own and the recipient's config.rangeDeletions, and informs itself and the recipient of the
 * migration's outcome by updating or deleting its own and the recipient's config.rangeDeletions
 * entries for the migration.
 */

(function() {
'use strict';

load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

function getNewNs(dbName) {
    if (typeof getNewNs.counter == 'undefined') {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

const dbName = "test";

var st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName}));
assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard0.shardName}));

function getCollectionUuidAndEpoch(ns) {
    const collectionDoc = st.s.getDB("config").getCollection("collections").findOne({_id: ns});
    assert.neq(null, collectionDoc);
    assert.neq(null, collectionDoc.uuid);
    assert.neq(null, collectionDoc.lastmodEpoch);
    return [collectionDoc.uuid, collectionDoc.lastmodEpoch];
}

function assertHasMigrationCoordinatorDoc({conn, ns, uuid, epoch}) {
    const query = {
        nss: ns,
        collectionUuid: uuid,
        donorShardId: st.shard0.shardName,
        recipientShardId: st.shard1.shardName,
        "range.min._id": MinKey,
        "range.max._id": MaxKey,
        "preMigrationChunkVersion.0": Timestamp(1, 0),
        "preMigrationChunkVersion.1": epoch
    };
    assert.neq(
        null,
        conn.getDB("config").getCollection("migrationCoordinators").findOne(query),
        "did not find document matching query " + tojson(query) +
            ", contents of config.migrationCoordinators on " + conn + ": " +
            tojson(conn.getDB("config").getCollection("migrationCoordinators").find().toArray()));
}

function assertEventuallyDoesNotHaveMigrationCoordinatorDoc(conn) {
    assert.soon(() => {
        return 0 == conn.getDB("config").getCollection("migrationCoordinators").find().itcount();
    });
}

function assertHasRangeDeletionDoc({conn, pending, whenToClean, ns, uuid}) {
    const query = {
        nss: ns,
        collectionUuid: uuid,
        donorShardId: st.shard0.shardName,
        "range.min._id": MinKey,
        "range.max._id": MaxKey,
        pending: (pending ? true : {$exists: false}),
        whenToClean: whenToClean
    };
    assert.neq(null,
               conn.getDB("config").getCollection("rangeDeletions").findOne(query),
               "did not find document matching query " + tojson(query) +
                   ", contents of config.rangeDeletions on " + conn + ": " +
                   tojson(conn.getDB("config").getCollection("rangeDeletions").find().toArray()));
}

function assertEventuallyDoesNotHaveRangeDeletionDoc(conn) {
    assert.soon(() => {
        return 0 == conn.getDB("config").getCollection("rangeDeletions").find().itcount();
    });
}

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Test end-to-end migration when migration commit succeeds, ns is " + ns);

    // Insert some docs into the collection.
    const numDocs = 1000;
    var bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the collection.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    const [uuid, epoch] = getCollectionUuidAndEpoch(ns);

    // Run the moveChunk asynchronously, pausing during cloning to allow the test to make
    // assertions.
    let step4Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            assert.commandWorked(db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
        }, ns, st.shard1.shardName), st.s.port);

    // Assert that the durable state for coordinating the migration was written correctly.
    step4Failpoint.wait();
    assertHasMigrationCoordinatorDoc({conn: st.shard0, ns, uuid, epoch});
    assertHasRangeDeletionDoc({conn: st.shard0, pending: true, whenToClean: "delayed", ns, uuid});
    assertHasRangeDeletionDoc({conn: st.shard1, pending: true, whenToClean: "now", ns, uuid});
    step4Failpoint.off();

    // Allow the moveChunk to finish.
    awaitResult();

    // Donor shard eventually cleans up the orphans.
    assert.soon(function() {
        return st.shard0.getDB(dbName).getCollection(collName).count() === 0;
    });
    assert.eq(numDocs, st.s.getDB(dbName).getCollection(collName).find().itcount());

    // The durable state for coordinating the migration is eventually cleaned up.
    assertEventuallyDoesNotHaveMigrationCoordinatorDoc(st.shard0);
    assertEventuallyDoesNotHaveRangeDeletionDoc(st.shard0);
    assertEventuallyDoesNotHaveRangeDeletionDoc(st.shard1);
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Test end-to-end migration when migration commit fails, ns is " + ns);

    // Insert some docs into the collection.
    const numDocs = 1000;
    var bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (var i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the collection.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    const [uuid, epoch] = getCollectionUuidAndEpoch(ns);

    // Turn on a failpoint to make the migration commit fail on the config server.
    let migrationCommitVersionErrorFailpoint =
        configureFailPoint(st.configRS.getPrimary(), "migrationCommitVersionError");

    // Run the moveChunk asynchronously, pausing during cloning to allow the test to make
    // assertions.
    let step4Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    let step5Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep5");
    const awaitResult = startParallelShell(
        funWithArgs(function(ns, toShardName) {
            // Expect StaleEpoch because of the failpoint that will make the migration commit fail.
            assert.commandFailedWithCode(
                db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                ErrorCodes.StaleEpoch);
        }, ns, st.shard1.shardName), st.s.port);

    // Assert that the durable state for coordinating the migration was written correctly.
    step4Failpoint.wait();
    assertHasMigrationCoordinatorDoc({conn: st.shard0, ns, uuid, epoch});
    assertHasRangeDeletionDoc({conn: st.shard0, pending: true, whenToClean: "delayed", ns, uuid});
    assertHasRangeDeletionDoc({conn: st.shard1, pending: true, whenToClean: "now", ns, uuid});
    step4Failpoint.off();

    // Assert that the recipient has 'numDocs' orphans.
    step5Failpoint.wait();
    assert.eq(numDocs, st.shard1.getDB(dbName).getCollection(collName).count());
    step5Failpoint.off();

    // Allow the moveChunk to finish.
    awaitResult();

    // Recipient shard eventually cleans up the orphans.
    assert.soon(function() {
        return st.shard1.getDB(dbName).getCollection(collName).count() === 0;
    });
    assert.eq(numDocs, st.s.getDB(dbName).getCollection(collName).find().itcount());

    // The durable state for coordinating the migration is eventually cleaned up.
    assertEventuallyDoesNotHaveMigrationCoordinatorDoc(st.shard0);
    assertEventuallyDoesNotHaveRangeDeletionDoc(st.shard0);
    assertEventuallyDoesNotHaveRangeDeletionDoc(st.shard1);

    migrationCommitVersionErrorFailpoint.off();
})();

st.stop();
})();
