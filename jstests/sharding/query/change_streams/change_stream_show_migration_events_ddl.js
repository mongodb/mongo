/**
 * Tests 'showMigrationEvents' behavior for DDL operations that cause data migration
 * (moveCollection, unshardCollection, moveChunk, movePrimary) and verifies the
 * 'changeStreamsEmitFromMigrate' server parameter.
 *
 * When 'showMigrationEvents: true' is set on a shard-level change stream, insert and delete events
 * triggered by migrations become visible. When 'changeStreamsEmitFromMigrate' is true (the
 * default), those events carry a 'fromMigrate: true' field. When the parameter is false the events
 * still appear but the 'fromMigrate' field is omitted.
 *
 * For 'movePrimary', untracked collections on the old primary shard are cloned onto the new primary
 * shard. This generates 'create' and 'createIndexes' DDL events on the recipient shard that also
 * carry 'fromMigrate: true' when 'changeStreamsEmitFromMigrate' is enabled.
 *
 * NOTE: 'showMigrationEvents: true' is only accepted on individual shard mongod nodes; mongos
 * rejects it with error 31123.
 *
 * @tags: [
 *   assumes_balancer_off,
 *   does_not_support_stepdowns,
 *   requires_fcv_90,
 *   requires_sharding,
 *   uses_change_streams,
 * ]
 */

import {ChangeStreamTest} from "jstests/libs/query/change_stream_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {runWithParamsAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";
import {describe, it, before, after} from "jstests/libs/mochalite.js";

// Returns a unique database name so each test starts with a clean slate.
let testIdx = 0;
const freshDbName = () => {
    return `test${testIdx++}`;
};

// Inserts a sentinel to delimit the stream, then reads change stream events one at a time until
// the sentinel is found. Returns all collected events, including the sentinel.
const collectEventsUntilSentinel = (coll, csTest, cursor) => {
    const sentinelId = "sentinel";
    assert.commandWorked(coll.insert({_id: sentinelId}));

    const isSentinel = (e) =>
        e.operationType === "insert" && e.ns.coll === coll.getName() && e.documentKey._id === sentinelId;

    const events = [];
    while (true) {
        const event = csTest.getOneChange(cursor);
        events.push(event);
        if (isSentinel(event)) {
            // The sentinel itself must not carry 'fromMigrate'.
            assert(!event.hasOwnProperty("fromMigrate"), `Sentinel must not have 'fromMigrate': ${tojson(event)}`);
            return events;
        }
    }
};

// Opens a DB-level change stream on shard1's primary and returns {csTest, cursor}. Creates an
// unsplittable collection on shard1 first (and immediately drops it) so that the database exists on
// the shard before the change stream is opened.
const openRecipientStream = (st, dbName, showExpandedEvents = false) => {
    const mongos = st.s;
    const db = mongos.getDB(dbName);

    // Ensure the database is initialized on shard1 before the change stream is opened.
    assert.commandWorked(db.runCommand({createUnsplittableCollection: "_warmup", dataShard: st.shard1.shardName}));
    assert.commandWorked(db.runCommand({drop: "_warmup"}));

    const shard1Primary = st.rs1.getPrimary();
    const csTest = new ChangeStreamTest(shard1Primary.getDB(dbName));
    const cursor = csTest.startWatchingChanges({
        pipeline: [
            {
                $changeStream: {
                    showMigrationEvents: true,
                    showSystemEvents: true,
                    showExpandedEvents,
                    allowToRunOnSystemNS: true,
                },
            },
        ],
        collection: 1,
    });
    return {csTest, cursor};
};

// Asserts that 'migrationInserts' (events into system.resharding.*) do not carry a 'fromMigrate'
// flag.
// This is intentional because 'fromMigrate' is about orphan documents, and resharding does not
// write orphan documents. All documents written by resharding are owned documents by the shards
// they are inserted into.
const assertMigrationInserts = (allEvents) => {
    const migrationInserts = allEvents.filter(
        (e) => e.operationType === "insert" && e.ns.coll.startsWith("system.resharding."),
    );
    assert.gt(
        migrationInserts.length,
        0,
        `Expected at least one migration insert from resharding machinery; ` + `all events: ${tojson(allEvents)}`,
    );

    migrationInserts.forEach((e) => {
        assert(!e.hasOwnProperty("fromMigrate"), `Expected no fromMigrate field on migration insert: ${tojson(e)}`);
    });
};

const assertReshardingDoneCatchUpAndRenameEvents = (allEvents, collName, emitFromMigrate) => {
    const catchUpEvents = allEvents.filter(
        (e) => e.operationType === "reshardDoneCatchUp" && e.ns.coll.startsWith("system.resharding."),
    );
    assert.eq(
        catchUpEvents.length,
        1,
        `Expected at least one 'reshardDoneCatchUp' event for '${collName}'; all events: ${tojson(allEvents)}`,
    );
    catchUpEvents.forEach((e) => {
        assert.eq(
            emitFromMigrate,
            e.hasOwnProperty("fromMigrate"),
            `Invalid fromMigrate on reshardDoneCatchUp event: ${tojson(e)}`,
        );
    });

    const renameEvents = allEvents.filter(
        (e) =>
            e.operationType === "rename" &&
            e.ns.coll.startsWith("system.resharding.") &&
            e.operationDescription.to.coll == collName,
    );
    assert.eq(
        renameEvents.length,
        1,
        `Expected at least one 'reshardDoneCatchUp' event for '${collName}'; all events: ${tojson(allEvents)}`,
    );
    renameEvents.forEach((e) => {
        assert.eq(
            emitFromMigrate,
            e.hasOwnProperty("fromMigrate"),
            `Invalid fromMigrate on rename event: ${tojson(e)}`,
        );
    });
};

describe("$changeStream showMigrationEvents", () => {
    let st;

    before(() => {
        st = new ShardingTest({
            shards: 2,
            rs: {nodes: 1, setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}},
        });
    });

    after(() => {
        st.stop();
    });

    describe("moveChunk/moveCollection/unshardCollection", () => {
        // ---------------------------------------------------------------------------
        // moveChunk
        // ---------------------------------------------------------------------------
        function runMoveChunkTest(emitFromMigrate) {
            const dbName = freshDbName();
            const collName = "coll";
            const mongos = st.s;
            const mongosColl = mongos.getCollection(`${dbName}.${collName}`);

            assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(mongos.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}));
            assert.commandWorked(mongosColl.insert({_id: 0}));
            assert.commandWorked(mongosColl.insert({_id: 20}));
            assert.commandWorked(mongos.adminCommand({split: `${dbName}.${collName}`, middle: {_id: 10}}));

            runWithParamsAllNonConfigNodes(st.s.getDB(dbName), {changeStreamsEmitFromMigrate: emitFromMigrate}, () => {
                // Open change streams on both shard primaries after the initial inserts.
                // The streams start from 'now' so they will not see the pre-migration inserts;
                // the first events they observe will be the migration events from moveChunk.
                const csTestShard0 = new ChangeStreamTest(st.shard0.getDB(dbName));
                const cursorShard0 = csTestShard0.startWatchingChanges({
                    pipeline: [{$changeStream: {showMigrationEvents: true}}],
                    collection: st.shard0.getCollection(`${dbName}.${collName}`),
                });
                const csTestShard1 = new ChangeStreamTest(st.shard1.getDB(dbName));
                const cursorShard1 = csTestShard1.startWatchingChanges({
                    pipeline: [{$changeStream: {showMigrationEvents: true}}],
                    collection: st.shard1.getCollection(`${dbName}.${collName}`),
                });

                assert.commandWorked(
                    mongos.adminCommand({
                        moveChunk: `${dbName}.${collName}`,
                        find: {_id: 20},
                        to: st.shard1.shardName,
                        _waitForDelete: true,
                    }),
                );

                // Donor shard (shard0) should see a delete for '{_id: 20}'.
                const donorDeleteEvent = csTestShard0.getOneChange(cursorShard0);
                assert.eq(donorDeleteEvent.operationType, "delete", tojson(donorDeleteEvent));
                assert.eq(donorDeleteEvent.documentKey, {_id: 20}, tojson(donorDeleteEvent));
                assert.eq(
                    emitFromMigrate,
                    donorDeleteEvent.hasOwnProperty("fromMigrate"),
                    `Invalid fromMigrate field value on donor delete: ${tojson(donorDeleteEvent)}`,
                );

                // Recipient shard (shard1) should see an insert for '{_id: 20}'.
                const recipientInsertEvent = csTestShard1.getOneChange(cursorShard1);
                assert.eq(recipientInsertEvent.operationType, "insert", tojson(recipientInsertEvent));
                assert.eq(recipientInsertEvent.documentKey, {_id: 20}, tojson(recipientInsertEvent));
                assert.eq(
                    emitFromMigrate,
                    recipientInsertEvent.hasOwnProperty("fromMigrate"),
                    `Invalid fromMigrate field value on recipient insert: ${tojson(recipientInsertEvent)}`,
                );

                csTestShard0.assertNoChange(cursorShard0);
                csTestShard1.assertNoChange(cursorShard1);

                csTestShard0.cleanUp();
                csTestShard1.cleanUp();
            });
        }

        it("moveChunk: migration events visible with fromMigrate:true", () => {
            runMoveChunkTest(true /* emitFromMigrate */);
        });

        it("moveChunk: migration events not visible with fromMigrate:false", () => {
            runMoveChunkTest(false /* emitFromMigrate */);
        });

        // ---------------------------------------------------------------------------
        // moveCollection
        // ---------------------------------------------------------------------------
        function runMoveCollectionTest(emitFromMigrate) {
            const dbName = freshDbName();
            const collName = "coll";
            const mongos = st.s;
            const db = mongos.getDB(dbName);
            const coll = db[collName];

            assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

            // Create an unsplittable (tracked, single-shard) collection on shard0.
            assert.commandWorked(
                db.runCommand({createUnsplittableCollection: collName, dataShard: st.shard0.shardName}),
            );
            for (let i = 0; i < 3; ++i) {
                assert.commandWorked(coll.insert({_id: i}));
            }

            // 'changeStreamsEmitFromMigrate' is evaluated when a change stream is opened, so the
            // parameter must be set before openRecipientStream is called.
            runWithParamsAllNonConfigNodes(db, {changeStreamsEmitFromMigrate: emitFromMigrate}, () => {
                // Watch shard1 (the recipient) at the DB level before the move.
                const {csTest, cursor} = openRecipientStream(st, dbName, true /* showExpandedEvents */);

                // Move the collection to shard1.
                assert.commandWorked(
                    mongos.adminCommand({moveCollection: `${dbName}.${collName}`, toShard: st.shard1.shardName}),
                );

                // After the move, inserts go to shard1.
                const events = collectEventsUntilSentinel(coll, csTest, cursor);

                // Migration inserts land in the temporary resharding namespace on shard1.
                assertMigrationInserts(events);

                assertReshardingDoneCatchUpAndRenameEvents(events, collName, emitFromMigrate);
                csTest.cleanUp();
            });

            db.dropDatabase();
        }

        it("moveCollection: recipient sees migration inserts with fromMigrate:true", () => {
            runMoveCollectionTest(true /* emitFromMigrate */);
        });

        it("moveCollection: recipient does not see migration inserts with fromMigrate:false", () => {
            runMoveCollectionTest(false /* emitFromMigrate */);
        });

        // ---------------------------------------------------------------------------
        // unshardCollection
        //
        // 'unshardCollection' converts a sharded collection into an unsplittable one on a specified
        // shard using the same resharding machinery, so the recipient shard also sees the cloned
        // documents as migration inserts.
        // ---------------------------------------------------------------------------
        function runUnshardCollectionTest(emitFromMigrate) {
            const dbName = freshDbName();
            const collName = "coll";
            const mongos = st.s;
            const db = mongos.getDB(dbName);
            const coll = db[collName];

            assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

            // Shard the collection with all chunks initially on shard0.
            assert.commandWorked(mongos.adminCommand({shardCollection: `${dbName}.${collName}`, key: {_id: 1}}));
            for (let i = 0; i < 3; ++i) {
                assert.commandWorked(coll.insert({_id: i}));
            }

            runWithParamsAllNonConfigNodes(db, {changeStreamsEmitFromMigrate: emitFromMigrate}, () => {
                // Watch shard1 (the recipient) at the DB level before the unshard.
                const {csTest, cursor} = openRecipientStream(st, dbName, true /* showExpandedEvents */);

                // Unshard the collection onto shard1.
                assert.commandWorked(
                    mongos.adminCommand({
                        unshardCollection: `${dbName}.${collName}`,
                        toShard: st.shard1.shardName,
                    }),
                );

                // After the unshard, inserts go to shard1.
                const events = collectEventsUntilSentinel(coll, csTest, cursor);
                const createEvents = events.filter(
                    (e) => e.operationType === "create" && e.ns.coll.startsWith("system.resharding."),
                );
                assert.eq(
                    createEvents.length,
                    1,
                    `Expected at least one 'create' event for '${collName}'; all events: ${tojson(events)}`,
                );
                createEvents.forEach((e) => {
                    assert.eq(
                        emitFromMigrate,
                        e.hasOwnProperty("fromMigrate"),
                        `Invalid fromMigrate on create event: ${tojson(e)}`,
                    );
                });

                // Migration inserts land in the temporary resharding namespace on shard1.
                assertMigrationInserts(events);

                assertReshardingDoneCatchUpAndRenameEvents(events, collName, emitFromMigrate);

                csTest.cleanUp();
            });

            db.dropDatabase();
        }

        it("unshardCollection: recipient sees migration inserts with fromMigrate:true", () => {
            runUnshardCollectionTest(true /* emitFromMigrate */);
        });

        it("unshardCollection: recipient does not see migration inserts with fromMigrate:false", () => {
            runUnshardCollectionTest(false /* emitFromMigrate */);
        });
    });

    // ---------------------------------------------------------------------------
    // reshardCollection
    //
    // 'reshardCollection' changes the shard key and redistributes data via the same resharding
    // machinery. By placing all initial chunks on shard0 and assigning the new key range
    // exclusively to shard1 via a zone, we force a deterministic cross-shard clone so that shard1
    // is always the recipient.
    // ---------------------------------------------------------------------------
    describe("reshardColllection", () => {
        function runReshardCollectionTest(emitFromMigrate) {
            const dbName = freshDbName();
            const collName = "coll";
            const mongos = st.s;
            const db = mongos.getDB(dbName);
            const coll = db[collName];
            const ns = `${dbName}.${collName}`;
            const zoneName = `zone_${dbName}`;

            assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));
            assert.commandWorked(mongos.adminCommand({shardCollection: ns, key: {_id: 1}}));

            // Insert documents with both old and new shard key fields.
            // All chunks start on shard0 because it is the primary shard.
            for (let i = 0; i < 3; ++i) {
                assert.commandWorked(coll.insert({_id: i, a: i}));
            }

            // Tag shard1 with a zone so that the 'zones' field in 'reshardCollection' forces all
            // chunks of the new key {a:1} onto shard1.
            assert.commandWorked(mongos.adminCommand({addShardToZone: st.shard1.shardName, zone: zoneName}));

            runWithParamsAllNonConfigNodes(db, {changeStreamsEmitFromMigrate: emitFromMigrate}, () => {
                // Watch shard1 (the recipient) at the DB level before the reshard.
                const {csTest, cursor} = openRecipientStream(st, dbName, true /* showExpandedEvents */);

                // Reshard to the new key {a:1}; the zone forces all chunks onto shard1.
                assert.commandWorked(
                    mongos.adminCommand({
                        reshardCollection: ns,
                        key: {a: 1},
                        numInitialChunks: 1,
                        zones: [{min: {a: MinKey}, max: {a: MaxKey}, zone: zoneName}],
                    }),
                );

                // After resharding, inserts go to shard1.
                const events = collectEventsUntilSentinel(coll, csTest, cursor);

                // Migration inserts land in the temporary resharding namespace on shard1.
                assertMigrationInserts(events);

                assertReshardingDoneCatchUpAndRenameEvents(events, collName, emitFromMigrate);

                csTest.cleanUp();
            });

            db.dropDatabase();

            // Remove the zone tag from shard1 (zone key ranges on the collection will be removed
            // when the database is dropped implicitly between tests).
            assert.commandWorked(mongos.adminCommand({removeShardFromZone: st.shard1.shardName, zone: zoneName}));
        }

        it("reshardCollection: recipient does not sees migration inserts with fromMigrate:true", () => {
            runReshardCollectionTest(true /* emitFromMigrate */);
        });

        it("reshardCollection: recipient does not sees migration inserts with fromMigrate:false", () => {
            runReshardCollectionTest(false /* emitFromMigrate */);
        });
    });

    // ---------------------------------------------------------------------------
    // movePrimary
    //
    // 'movePrimary' moves the primary shard of a database. Untracked collections on the old primary
    // are cloned onto the new primary via the migration path, generating 'create' and
    // 'createIndexes' DDL events on the recipient shard.
    // ---------------------------------------------------------------------------
    describe("movePrimary", () => {
        function runMovePrimaryTest(emitFromMigrate) {
            const dbName = freshDbName();
            const collName = "coll";
            const mongos = st.s;
            const db = mongos.getDB(dbName);

            // Create the database with shard0 as primary.
            assert.commandWorked(mongos.adminCommand({enableSharding: dbName, primaryShard: st.shard0.shardName}));

            // Create an untracked collection directly on shard0 (bypassing mongos so it is not
            // registered in the sharding catalog, making it eligible for relocation by movePrimary).
            assert.commandWorked(db.createCollection(collName));
            for (let i = 0; i < 3; ++i) {
                assert.commandWorked(db[collName].insert({_id: i}));
            }
            assert.commandWorked(db[collName].createIndexes([{a: 1}]));

            runWithParamsAllNonConfigNodes(db, {changeStreamsEmitFromMigrate: emitFromMigrate}, () => {
                // Watch shard1 (the recipient) at the DB level before the move.
                const {csTest, cursor} = openRecipientStream(st, dbName, true /* showExpandedEvents */);

                // Move the primary shard from shard0 to shard1.
                assert.commandWorked(mongos.adminCommand({movePrimary: dbName, to: st.shard1.shardName}));

                // After the move the collection lives on shard1 (inserts through mongos now route
                // to shard1 as the new primary).
                const events = collectEventsUntilSentinel(db[collName], csTest, cursor);

                // movePrimary clones the collection onto the recipient shard, which must emit a
                // 'create' event. The event must carry 'fromMigrate: true' iff emitFromMigrate is
                // enabled.
                const createEvents = events.filter((e) => e.operationType === "create" && e.ns.coll === collName);
                assert.gt(
                    createEvents.length,
                    0,
                    `Expected at least one 'create' event for '${collName}'; all events: ${tojson(events)}`,
                );
                createEvents.forEach((e) => {
                    assert.eq(
                        emitFromMigrate,
                        e.hasOwnProperty("fromMigrate"),
                        `Invalid fromMigrate on create event: ${tojson(e)}`,
                    );
                });

                // movePrimary also creates the collection's indexes on the recipient shard, which
                // must emit a 'createIndexes' event with the same fromMigrate semantics.
                const createIndexesEvents = events.filter(
                    (e) => e.operationType === "createIndexes" && e.ns.coll === collName,
                );
                assert.gt(
                    createIndexesEvents.length,
                    0,
                    `Expected at least one 'createIndexes' event for '${collName}'; all events: ${tojson(events)}`,
                );
                createIndexesEvents.forEach((e) => {
                    assert.eq(
                        emitFromMigrate,
                        e.hasOwnProperty("fromMigrate"),
                        `Invalid fromMigrate on createIndexes event: ${tojson(e)}`,
                    );
                });

                csTest.cleanUp();
            });

            db.dropDatabase();
        }

        it("movePrimary: recipient sees 'create' and 'createIndexes' events with fromMigrate:true", () => {
            runMovePrimaryTest(true /* emitFromMigrate */);
        });

        it("movePrimary: recipient sees 'create' and 'createIndexes' events without fromMigrate:false", () => {
            runMovePrimaryTest(false /* emitFromMigrate */);
        });
    });
});
