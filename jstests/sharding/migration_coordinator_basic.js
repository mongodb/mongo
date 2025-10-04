/**
 * Tests that a donor shard durably records a migration's state, inserts pending entries into its
 * own and the recipient's config.rangeDeletions, and informs itself and the recipient of the
 * migration's outcome by updating or deleting its own and the recipient's config.rangeDeletions
 * entries for the migration.
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {funWithArgs} from "jstests/libs/parallel_shell_helpers.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ShardVersioningUtil} from "jstests/sharding/libs/shard_versioning_util.js";

function is81orAbove() {
    // Requires all primary shard nodes to be running the fcvRequired version.
    let isFcvGreater = true;
    st.forEachConnection(function (conn) {
        const fcvDoc = conn.adminCommand({getParameter: 1, featureCompatibilityVersion: 1});
        if (MongoRunner.compareBinVersions(fcvDoc.featureCompatibilityVersion.version, "8.1") < 0) {
            isFcvGreater = false;
        }
    });
    return isFcvGreater;
}

function getNewNs(dbName) {
    if (typeof getNewNs.counter == "undefined") {
        getNewNs.counter = 0;
    }
    getNewNs.counter++;
    const collName = "ns" + getNewNs.counter;
    return [collName, dbName + "." + collName];
}

const dbName = "test";

var st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

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
        $or: [
            {"preMigrationChunkVersion.0": Timestamp(1, 0)},
            {"preMigrationChunkVersion.1": epoch},
            {"preMigrationChunkVersion.v": Timestamp(1, 0)},
            {"preMigrationChunkVersion.e": epoch},
        ],
    };
    assert.neq(
        null,
        conn.getDB("config").getCollection("migrationCoordinators").findOne(query),
        "did not find document matching query " +
            tojson(query) +
            ", contents of config.migrationCoordinators on " +
            conn +
            ": " +
            tojson(conn.getDB("config").getCollection("migrationCoordinators").find().toArray()),
    );
}

function assertEventuallyDoesNotHaveMigrationCoordinatorDoc(conn) {
    assert.soon(() => {
        return 0 == conn.getDB("config").getCollection("migrationCoordinators").find().itcount();
    });
}

function assertHasRangeDeletionDoc({conn, pending, whenToClean, ns, uuid, processing, preMigrationShardVersion}) {
    const query = {
        nss: ns,
        collectionUuid: uuid,
        donorShardId: st.shard0.shardName,
        "range.min._id": MinKey,
        "range.max._id": MaxKey,
        whenToClean: whenToClean,
    };

    const doc = conn.getDB("config").getCollection("rangeDeletions").findOne(query);
    assert.neq(
        null,
        doc,
        "did not find document matching query " +
            tojson(query) +
            ", contents of config.rangeDeletions on " +
            conn +
            ": " +
            tojson(conn.getDB("config").getCollection("rangeDeletions").find().toArray()),
    );
    if (pending) {
        assert.eq(
            pending,
            doc.pending,
            "Unexpected value on `pending` field. Range deletion doc found: " + tojson(doc),
        );
    } else {
        assert(
            !doc.hasOwnProperty("pending"),
            "Field `pending` was not expected to be present. Range deletion doc found: " + tojson(doc),
        );
    }
    if (processing) {
        assert.eq(
            processing,
            doc.processing,
            "Unexpected value on `processing` field. Range deletion doc found: " + tojson(doc),
        );
    } else {
        assert(
            !doc.hasOwnProperty("processing"),
            "Field `processing` was not expected to be present. Range deletion doc found: " + tojson(doc),
        );
    }
    if (is81orAbove()) {
        assert.eq(
            preMigrationShardVersion,
            doc.preMigrationShardVersion,
            "Unexpected value on `preMigrationShardVersion` field. Range deletion doc found: " + tojson(doc),
        );
    }
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
    let bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the collection.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    const [uuid, epoch] = getCollectionUuidAndEpoch(ns);
    const preMigrationShardVersion = ShardVersioningUtil.getShardVersion(st.shard0, ns, true /* waitForRefresh */);

    // Run the moveChunk asynchronously, pausing during cloning to allow the test to make
    // assertions.
    let step4Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    const awaitResult = startParallelShell(
        funWithArgs(
            function (ns, toShardName) {
                assert.commandWorked(db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}));
            },
            ns,
            st.shard1.shardName,
        ),
        st.s.port,
    );

    // Assert that the durable state for coordinating the migration was written correctly.
    step4Failpoint.wait();
    assertHasMigrationCoordinatorDoc({conn: st.shard0, ns, uuid, epoch});
    assertHasRangeDeletionDoc({
        conn: st.shard0,
        pending: true,
        whenToClean: "delayed",
        ns,
        uuid,
        processing: false,
        preMigrationShardVersion,
    });
    assertHasRangeDeletionDoc({
        conn: st.shard1,
        pending: true,
        whenToClean: "now",
        ns,
        uuid,
        processing: false,
        preMigrationShardVersion: ShardVersioningUtil.kIgnoredShardVersion,
    });
    step4Failpoint.off();

    // Allow the moveChunk to finish.
    awaitResult();

    // Donor shard eventually cleans up the orphans.
    assert.soon(function () {
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
    jsTest.log("Test end-to-end migration when migration commit fails due to StaleConfig, ns is " + ns);

    // Insert some docs into the collection.
    const numDocs = 1000;
    let bulk = st.s.getDB(dbName).getCollection(collName).initializeUnorderedBulkOp();
    for (let i = 0; i < numDocs; i++) {
        bulk.insert({_id: i});
    }
    assert.commandWorked(bulk.execute());

    // Shard the collection.
    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {_id: 1}}));
    const [uuid, epoch] = getCollectionUuidAndEpoch(ns);
    const preMigrationShardVersion = ShardVersioningUtil.getShardVersion(st.shard0, ns, true /* waitForRefresh */);

    // Turn on a failpoint to make the migration commit fail on the config server.
    let migrationCommitVersionErrorFailpoint = configureFailPoint(
        st.configRS.getPrimary(),
        "migrationCommitVersionError",
    );

    // Run the moveChunk asynchronously, pausing during cloning to allow the test to make
    // assertions.
    let step4Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep4");
    let step5Failpoint = configureFailPoint(st.shard0, "moveChunkHangAtStep5");
    const awaitResult = startParallelShell(
        funWithArgs(
            function (ns, toShardName) {
                // Expect StaleEpoch because of the failpoint that will make the migration commit fail.
                assert.commandFailedWithCode(
                    db.adminCommand({moveChunk: ns, find: {_id: 0}, to: toShardName}),
                    ErrorCodes.StaleEpoch,
                );
            },
            ns,
            st.shard1.shardName,
        ),
        st.s.port,
    );

    // Assert that the durable state for coordinating the migration was written correctly.
    step4Failpoint.wait();
    assertHasMigrationCoordinatorDoc({conn: st.shard0, ns, uuid, epoch});
    assertHasRangeDeletionDoc({
        conn: st.shard0,
        pending: true,
        whenToClean: "delayed",
        ns,
        uuid,
        processing: false,
        preMigrationShardVersion,
    });
    assertHasRangeDeletionDoc({
        conn: st.shard1,
        pending: true,
        whenToClean: "now",
        ns,
        uuid,
        processing: false,
        preMigrationShardVersion: ShardVersioningUtil.kIgnoredShardVersion,
    });
    step4Failpoint.off();

    // Assert that the recipient has 'numDocs' orphans.
    step5Failpoint.wait();
    assert.eq(numDocs, st.shard1.getDB(dbName).getCollection(collName).count());
    step5Failpoint.off();

    // Allow the moveChunk to finish.
    awaitResult();

    // Recipient shard eventually cleans up the orphans.
    assert.soon(function () {
        return st.shard1.getDB(dbName).getCollection(collName).count() === 0;
    });
    assert.eq(numDocs, st.s.getDB(dbName).getCollection(collName).find().itcount());

    // The durable state for coordinating the migration is eventually cleaned up.
    assertEventuallyDoesNotHaveMigrationCoordinatorDoc(st.shard0);
    assertEventuallyDoesNotHaveRangeDeletionDoc(st.shard0);
    assertEventuallyDoesNotHaveRangeDeletionDoc(st.shard1);

    migrationCommitVersionErrorFailpoint.off();
})();

(() => {
    const [collName, ns] = getNewNs(dbName);
    jsTest.log("Test end-to-end migration when migration commit fails to due to invalid chunk query, ns is " + ns);

    assert.commandWorked(st.s.adminCommand({shardCollection: ns, key: {x: 1}}));
    const invalidChunkQueryFailPoint = configureFailPoint(st.configRS.getPrimary(), "migrateCommitInvalidChunkQuery");

    assert.commandFailedWithCode(
        st.s.adminCommand({moveChunk: ns, find: {x: MinKey}, to: st.shard1.shardName}),
        ErrorCodes.UpdateOperationFailed,
    );

    invalidChunkQueryFailPoint.off();
})();

st.stop();
