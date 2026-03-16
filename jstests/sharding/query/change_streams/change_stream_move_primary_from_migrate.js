/**
 * Tests that movePrimary marks createCollection and createIndexes oplog entries with
 * fromMigrate: true on the recipient shard. These events are hidden from change streams
 * by default but become visible when showSystemEvents is enabled.
 *
 * Verifies behavior across all three collection types (untracked, unsplittable, sharded)
 * under a single database using a database-level change stream.
 *
 * NOTE: The showSystemEvents:true test case forces version "v1" because v2 precise shard
 * targeting misses fromMigrate events on the recipient shard. See the inline comment in
 * testChangeStreamEventsEmittedByMovePrimaryCommand() for details.
 *
 * @tags: [
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */
import {describe, it, before, beforeEach, afterEach, after} from "jstests/libs/mochalite.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";

describe("$changeStream", function () {
    let st;
    let db;
    let csTest;
    const dbName = jsTestName();
    const untrackedCollName = "untracked";
    const unsplittableCollName = "unsplittable";
    const shardedCollName = "sharded";

    before(function () {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
    });

    beforeEach(function () {
        db = st.s.getDB(dbName);
        assert.commandWorked(db.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
    });

    afterEach(function () {
        csTest.cleanUp();
        assert.commandWorked(db.dropDatabase());
    });

    after(function () {
        st.stop();
    });

    /**
     * Runs a movePrimary test with the given 'showSystemEvents' setting.
     *
     * Setup: creates three collections under the same database on shard0:
     *   - untracked: with a secondary index and a document (fully cloned by movePrimary)
     *   - unsplittable: tracked, with a secondary index (only collection shell is cloned,
     *     indexes and data are skipped)
     *   - sharded: tracked (only collection shell is cloned, indexes and data are skipped)
     *
     * Opens a database-level change stream, executes movePrimary to shard1, then inserts
     * a sentinel document into the untracked collection.
     *
     * When showSystemEvents is false: only the sentinel insert is visible.
     * When showSystemEvents is true: the fromMigrate:true create and createIndexes events
     * from the cloner on the recipient shard become visible before the sentinel insert.
     */
    function testChangeStreamEventsEmittedByMovePrimaryCommand(showSystemEvents) {
        // Create untracked collection with a secondary index and a document.
        assert.commandWorked(db.createCollection(untrackedCollName));
        assert.commandWorked(db[untrackedCollName].createIndex({x: 1}));
        assert.commandWorked(db[untrackedCollName].insertOne({_id: 0, data: "pre_move"}));

        // Create unsplittable collection (tracked) with a secondary index.
        // Even though it has a secondary index, the cloner skips index copying for
        // tracked collections, so no createIndexes event should appear for it.
        assert.commandWorked(
            db.runCommand({
                createUnsplittableCollection: unsplittableCollName,
                dataShard: st.shard0.shardName,
            }),
        );
        assert.commandWorked(db[unsplittableCollName].createIndex({y: 1}));

        // Create sharded collection.
        assert.commandWorked(db.createCollection(shardedCollName));
        assert.commandWorked(db[shardedCollName].createIndex({data: 1}));
        assert.commandWorked(st.s.adminCommand({shardCollection: `${dbName}.${shardedCollName}`, key: {data: 1}}));

        // Open a database-level change stream with showExpandedEvents (required to see
        // create/createIndexes event types) and the parameterized showSystemEvents setting.
        //
        // When showSystemEvents is true, we force version "v1" to work around a limitation
        // in v2 precise shard targeting.
        //
        // With v2, mongos only opens change stream cursors on shards returned by placement
        // history. Before movePrimary, the recipient shard (shard1) has no data for this
        // database, so v2 does not open a cursor there. The fromMigrate:true
        // create/createIndexes events are written to shard1's oplog during the clone phase
        // — before the v2 reader detects the movePrimary control event and opens a new
        // cursor on shard1 at clusterTime+1 (after the control event), missing the earlier
        // fromMigrate events. With v1, cursors are opened on all shards from the start, so
        // shard1's cursor captures those events as they are written.
        //
        // When showSystemEvents is false, fromMigrate events are filtered out regardless of
        // v1/v2, so we let the suite override control the version.
        const csOptions = {showExpandedEvents: true, showSystemEvents: showSystemEvents};
        if (showSystemEvents) {
            csOptions.version = "v1";
        }
        csTest = new ChangeStreamTest(db);
        const cursor = csTest.startWatchingChanges({
            pipeline: [{$changeStream: csOptions}],
            collection: 1,
        });

        // Execute movePrimary to shard1.
        assert.commandWorked(st.s.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

        // Insert a sentinel document after movePrimary to anchor the stream.
        const sentinelDoc = {_id: 1, data: "sentinel"};
        assert.commandWorked(db[untrackedCollName].insertOne(sentinelDoc));

        if (showSystemEvents) {
            // TODO SERVER-88167: Once fromMigrate is exposed in change stream event
            // documents, add fromMigrate: true to these expected events to directly
            // verify the flag.
            //
            // movePrimary replays create and createIndexes on the recipient shard.
            // Untracked collections get both create + createIndexes (full clone).
            // Tracked collections (unsplittable, sharded) get only create
            // (the cloner skips index copying for tracked collections).
            // Order across collections is unpredictable, so assert unordered.
            csTest.assertNextChangesEqualUnordered({
                cursor: cursor,
                expectedChanges: [
                    {operationType: "create", ns: {db: dbName, coll: untrackedCollName}},
                    {operationType: "createIndexes", ns: {db: dbName, coll: untrackedCollName}},
                    {operationType: "create", ns: {db: dbName, coll: unsplittableCollName}},
                    {operationType: "create", ns: {db: dbName, coll: shardedCollName}},
                ],
            });
        }

        // The sentinel insert is always visible and arrives after migration events.
        csTest.assertNextChangesEqual({
            cursor: cursor,
            expectedChanges: [
                {
                    operationType: "insert",
                    ns: {db: dbName, coll: untrackedCollName},
                    fullDocument: sentinelDoc,
                    documentKey: {_id: sentinelDoc._id},
                },
            ],
        });
        csTest.assertNoChange(cursor);
    }

    it("hides fromMigrate events from movePrimary when showSystemEvents is false", function () {
        testChangeStreamEventsEmittedByMovePrimaryCommand(false /* showSystemEvents */);
    });

    it("shows fromMigrate events from movePrimary when showSystemEvents is true", function () {
        testChangeStreamEventsEmittedByMovePrimaryCommand(true /* showSystemEvents */);
    });
});
