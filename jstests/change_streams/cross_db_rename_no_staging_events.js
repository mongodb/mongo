/**
 * SERVER-107688: cross-database renameCollection must not leak the
 * data-cloning phase's staging-collection oplog entries into change-stream
 * consumers. The rename copies the source collection into a temporary
 * "tmp<nonce>.renameCollection" namespace on the destination database, then
 * atomically renames the staging collection into place. Today, the create,
 * createIndexes and per-document insert oplog entries emitted while populating
 * the staging collection are observable to:
 *   - database-scoped change streams on the destination DB
 *   - cluster-scoped change streams (allChangesForCluster: true)
 *
 * That leak surprises mongosync, CDC pipelines and any consumer that resumes
 * against a temporary namespace that disappears at the end of the rename. The
 * fix marks the data-cloning phase's oplog entries with fromMigrate:true so
 * that the change-stream parser (buildNotFromMigrateFilter() in
 * change_stream_filter_helpers.cpp) drops them by default.
 *
 * This test asserts the post-fix behaviour: a cross-DB rename produces a
 * single "rename" event on the destination DB stream (and on the cluster
 * stream) -- the staging namespace's create / createIndexes / insert events
 * are not emitted. The pre-existing ddl_rename_cross_db.js covers the source
 * DB events and the user-facing rename event; this test covers only the
 * staging-leak invariant.
 *
 * @tags: [
 *   requires_fcv_60,
 *   uses_change_streams,
 *   # Cross-DB rename is not directly supported through mongos in sharded
 *   # configurations; the source / destination may live on different shards.
 *   assumes_against_mongod_not_mongos,
 * ]
 */
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

const kStagingNsRegex = /^tmp[a-zA-Z0-9]{5}\.renameCollection$/;

function eventIsForStagingNamespace(event) {
    if (!event || !event.ns || typeof event.ns.coll !== "string") {
        return false;
    }
    return kStagingNsRegex.test(event.ns.coll);
}

function assertNoStagingEvents(events, label) {
    for (const event of events) {
        assert(
            !eventIsForStagingNamespace(event),
            () =>
                "SERVER-107688 leak: " +
                label +
                " stream observed an event for the cross-DB rename staging" +
                " namespace -- got " +
                tojson(event),
        );
    }
}

function runScenario({watchScope, dropTarget}) {
    const suffix = "_" + watchScope.substr(0, 3) + "_" + (dropTarget ? "drop" : "newdb");
    const dstDb = db.getSiblingDB(jsTestName() + "_dst" + suffix);
    const srcDb = dstDb.getSiblingDB(jsTestName() + "_src" + suffix);
    const dstColl = dstDb.getCollection("target");
    const srcColl = srcDb.getCollection("source");

    assert.commandWorked(srcDb.dropDatabase());
    assert.commandWorked(dstDb.dropDatabase());

    // Ensure the destination DB exists before watching it. Otherwise the
    // database-scoped stream will fail to open against an absent database.
    assert.commandWorked(dstDb.createCollection("anchor"));

    if (dropTarget) {
        // Pre-create the destination collection so dropTarget:true has work to do.
        assert.commandWorked(dstColl.insertMany([{b: 1}, {b: 2}]));
        assert.commandWorked(dstColl.createIndex({b: 1}));
    }

    // Populate the source. The data-cloning phase of the rename re-emits
    // these into the destination's staging namespace; that is the leak.
    assert.commandWorked(srcColl.insertMany([{key0: "v0"}, {key1: "v1"}, {key1: "v2"}]));
    assert.commandWorked(srcColl.createIndex({key0: 1}));
    assert.commandWorked(srcColl.createIndex({key1: 1}));

    const pipeline = [{$changeStream: {showExpandedEvents: true}}];
    let cursor;
    let streamLabel;

    if (watchScope === "database") {
        const testStream = new ChangeStreamTest(dstDb);
        cursor = testStream.startWatchingChanges({pipeline, collection: /*all colls=*/ 1});
        streamLabel = "database-scoped";

        // Execute the cross-DB rename. The admin command is the canonical
        // entry point; runCommand on either DB works against mongod.
        const renameSpec = {renameCollection: srcColl.getFullName(), to: dstColl.getFullName()};
        if (dropTarget) {
            renameSpec.dropTarget = true;
        }
        assert.commandWorked(dstDb.adminCommand(renameSpec));

        // Drain a generous prefix of events; we expect at most the final
        // user-facing "rename" event on the destination namespace, plus any
        // pre-existing destination-collection traffic from dropTarget.
        const drained = testStream.getNextChanges(cursor, 1);
        assertNoStagingEvents(drained, streamLabel);

        // The single drained event must be the public "rename" -- not an
        // insert or createIndexes attributed to the staging namespace.
        assert.eq(
            drained[0].operationType,
            "rename",
            "expected user-facing rename event, got: " + tojson(drained[0]),
        );
        assert.eq(drained[0].ns.db, dstDb.getName());
        assert.eq(drained[0].ns.coll, dstColl.getName());
    } else if (watchScope === "cluster") {
        const adminDb = db.getSiblingDB("admin");
        const csCursor = adminDb.aggregate(
            [{$changeStream: {showExpandedEvents: true, allChangesForCluster: true}}],
            {cursor: {batchSize: 0}},
        );
        streamLabel = "cluster-scoped";

        const renameSpec = {renameCollection: srcColl.getFullName(), to: dstColl.getFullName()};
        if (dropTarget) {
            renameSpec.dropTarget = true;
        }
        assert.commandWorked(dstDb.adminCommand(renameSpec));

        // Pull a bounded set of events. Cluster streams will also see
        // legitimate source-DB events (insert, createIndexes, drop) plus the
        // public rename -- but none of them should reference the staging ns.
        const observed = [];
        assert.soon(
            () => {
                while (csCursor.hasNext()) {
                    const event = csCursor.next();
                    observed.push(event);
                    if (event.operationType === "rename" && event.ns.db === dstDb.getName()) {
                        return true;
                    }
                }
                return false;
            },
            () => "cluster stream never observed the public rename event; saw " + tojson(observed),
            /*timeout=*/ 30 * 1000,
        );
        assertNoStagingEvents(observed, streamLabel);
        csCursor.close();
    }

    // Tidy up so re-runs of the suite are deterministic.
    assert.commandWorked(srcDb.dropDatabase());
    assert.commandWorked(dstDb.dropDatabase());
}

for (const watchScope of ["database", "cluster"]) {
    for (const dropTarget of [false, true]) {
        jsTestLog("cross_db_rename_no_staging_events: " + watchScope + " dropTarget=" + dropTarget);
        runScenario({watchScope, dropTarget});
    }
}
