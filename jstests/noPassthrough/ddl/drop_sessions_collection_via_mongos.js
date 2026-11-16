/**
 * Verifies that dropping config.system.sessions through mongos is rejected by default and allowed
 * only when the runtime server parameter is enabled.
 *
 * @tags: [
 *   requires_sharding,
 *   multiversion_incompatible,
 *   # refreshLogicalSessionCacheNow is sent directly to the config primary. A concurrent
 *   # config-server stepdown can make the command fail with NotWritablePrimary.
 *   does_not_support_stepdowns,
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {after, afterEach, before, beforeEach, describe, it} from "jstests/libs/mochalite.js";
import {Thread} from "jstests/libs/parallelTester.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {removeShard} from "jstests/sharding/libs/remove_shard_util.js";

// The test deliberately creates local/global UUID mismatches before cleaning them up.
TestData.skipCheckMetadataConsistency = true;

describe("drop config.system.sessions via mongos", function () {
    const kSessionsNs = "config.system.sessions";
    const kTestDbs = [
        "crud_after_drop",
        "read_after_drop",
        "transaction_after_drop",
        "retryable_write_after_drop",
    ];
    // Name the extra shard so it sorts first alphabetically and becomes the
    // "first shard" once added.
    const kExtraShardName = "aaaa";

    let st;
    let extraShard;

    function assertSessionsCollectionTracked() {
        assert.soon(() => {
            return st.s.getDB("config").collections.findOne({_id: kSessionsNs}) !== null;
        }, "config.system.sessions was not tracked in time");
    }

    function refreshSessions() {
        assert.commandWorked(
            st.configRS.getPrimary().adminCommand({refreshLogicalSessionCacheNow: 1}),
        );
        assertSessionsCollectionTracked();
    }

    function getSessionsCollection() {
        const sessions = st.s.getDB("config").collections.findOne({_id: kSessionsNs});
        assert.neq(null, sessions, "config.system.sessions was not tracked");
        return sessions;
    }

    function getDataShardPrimaries() {
        const shardPrimaries = st._rs.map((rs) => rs.test.getPrimary());
        if (extraShard) {
            shardPrimaries.push(extraShard.getPrimary());
        }
        return shardPrimaries;
    }

    function assertNoSessionsCollectionOnServer(server) {
        assert.eq(
            0,
            server.getDB("config").getCollectionInfos({name: "system.sessions"}).length,
            "system.sessions should not exist",
            {host: server.host},
        );
    }

    function assertNoSessionsShardCatalogOnServer(server, sessionsUUID) {
        const configDB = server.getDB("config");
        assert.eq(
            null,
            configDB.getCollection("shard.catalog.collections").findOne({_id: kSessionsNs}),
            "sessions metadata should be removed from the shard catalog",
            {host: server.host},
        );
        assert.eq(
            0,
            configDB.getCollection("shard.catalog.chunks").countDocuments({uuid: sessionsUUID}),
            "sessions chunk metadata should be removed from the shard catalog",
            {host: server.host, sessionsUUID},
        );
    }

    function assertSessionsCollectionDropped(sessionsUUID) {
        const configDB = st.s.getDB("config");
        assert.eq(null, configDB.collections.findOne({_id: kSessionsNs}));
        assert.eq(
            0,
            configDB.chunks.countDocuments({uuid: sessionsUUID}),
            "sessions chunks should be removed from the global catalog",
            {sessionsUUID},
        );

        assertNoSessionsCollectionOnServer(st.configRS.getPrimary());
        assertNoSessionsShardCatalogOnServer(st.configRS.getPrimary(), sessionsUUID);
        for (const shardPrimary of getDataShardPrimaries()) {
            assertNoSessionsCollectionOnServer(shardPrimary);
            assertNoSessionsShardCatalogOnServer(shardPrimary, sessionsUUID);
        }
    }

    // Drop sessions collection, run the callaback and re-create it.
    // The helper enables a failpoint to prevent re-creation of the sessions collection
    // so that the after-drop state can be tested in isolation.
    function withSessionsCollectionDropped(callback) {
        const sessionsUUID = getSessionsCollection().uuid;

        setPreventSessionsCollectionSharding(true);
        try {
            assert.commandWorked(st.s.getDB("config").runCommand({drop: "system.sessions"}));
            assertSessionsCollectionDropped(sessionsUUID);
            callback();
        } finally {
            setPreventSessionsCollectionSharding(false);
            refreshSessions();
        }
    }

    function dropSessionsCollection() {
        withSessionsCollectionDropped(function () {});
    }

    function dropTestDatabases() {
        for (const dbName of kTestDbs) {
            st.s.getDB(dbName).dropDatabase();
        }
    }

    // Best-effort cleanup used when a drop leaves local or shard-catalog leftovers.
    function dropLocalSessionsLeftovers(sessionsUUID) {
        const servers = getDataShardPrimaries();
        servers.push(st.configRS.getPrimary());
        for (const server of servers) {
            const configDB = server.getDB("config");
            configDB.runCommand({drop: "system.sessions"});
            configDB.getCollection("shard.catalog.collections").deleteOne({_id: kSessionsNs});
            configDB.getCollection("shard.catalog.chunks").deleteMany({uuid: sessionsUUID});
        }
        const configDB = st.s.getDB("config");
        configDB.getCollection("collections").deleteOne({_id: kSessionsNs});
        configDB.getCollection("chunks").deleteMany({uuid: sessionsUUID});
    }

    function setAllowDrop(enabled) {
        assert.commandWorked(
            st.s.adminCommand({setParameter: 1, allowDropSystemSessionCollection: enabled}),
        );
    }

    function setPreventSessionsCollectionSharding(enabled) {
        assert.commandWorked(
            st.configRS.getPrimary().adminCommand({
                configureFailPoint: "preventSessionsCollectionSharding",
                mode: enabled ? "alwaysOn" : "off",
            }),
        );
    }

    function startExtraShard() {
        extraShard = new ReplSetTest({name: kExtraShardName, nodes: 1});
        extraShard.startSet({shardsvr: ""});
        extraShard.initiate();
        assert.commandWorked(
            st.s.adminCommand({addShard: extraShard.getURL(), name: kExtraShardName}),
        );
    }

    function stopExtraShard() {
        if (extraShard) {
            // Drop test databases so removeShard doesn't get stuck draining
            // unsharded collections (the balancer is off).
            dropTestDatabases();
            // If the sessions chunk is on the extra shard, move it back to rs0
            // before removing so removeShard doesn't get stuck (balancer is off).
            const configDB = st.s.getDB("config");
            const sessions = configDB.collections.findOne({_id: kSessionsNs});
            if (sessions) {
                const chunk = configDB.chunks.findOne({uuid: sessions.uuid});
                if (chunk && chunk.shard === kExtraShardName) {
                    assert.commandWorked(
                        st.s.adminCommand({
                            moveChunk: kSessionsNs,
                            find: {_id: 0},
                            to: st.shard0.shardName,
                        }),
                    );
                }
            }
            removeShard(st, kExtraShardName);
            extraShard.stopSet();
            extraShard = null;
        }
    }

    function runCrudAfterDrop() {
        const crudDB = st.s.getDB("crud_after_drop");
        assert.commandWorked(crudDB.createCollection("coll"));

        withSessionsCollectionDropped(function () {
            assert.commandWorked(crudDB.coll.insert({_id: 1, value: 1}));
            assert.commandWorked(crudDB.coll.update({_id: 1}, {$set: {value: 2}}));
            assert.commandWorked(crudDB.coll.remove({_id: 1}));
            assert.eq(null, crudDB.coll.findOne({_id: 1}));
        });
    }

    function runReadsAfterDrop() {
        const readDB = st.s.getDB("read_after_drop");
        assert.commandWorked(readDB.createCollection("coll"));
        assert.commandWorked(readDB.coll.insert({_id: 1, value: 1}));

        withSessionsCollectionDropped(function () {
            const readResult = assert.commandWorked(
                readDB.runCommand({find: "coll", filter: {_id: 1}}),
            );
            assert.eq(1, readResult.cursor.firstBatch.length);
            assert.eq({_id: 1, value: 1}, readResult.cursor.firstBatch[0]);
        });
    }

    function runTransactionsAfterDrop() {
        const transactionDB = st.s.getDB("transaction_after_drop");
        assert.commandWorked(transactionDB.createCollection("coll"));

        withSessionsCollectionDropped(function () {
            const transactionSession = st.s.startSession();
            const sessionDB = transactionSession.getDatabase("transaction_after_drop");
            transactionSession.startTransaction();
            assert.commandWorked(sessionDB.coll.insert({_id: 1, value: 1}));
            transactionSession.commitTransaction();
            assert.eq({_id: 1, value: 1}, transactionDB.coll.findOne({_id: 1}));
            transactionSession.endSession();
        });
    }

    function runRetryableWritesAfterDrop() {
        const retryableWriteDB = st.s.getDB("retryable_write_after_drop");
        assert.commandWorked(retryableWriteDB.createCollection("coll"));

        withSessionsCollectionDropped(function () {
            const retryableWriteSession = st.s.startSession({retryWrites: true});
            const retryableWriteSessionDB = retryableWriteSession.getDatabase(
                "retryable_write_after_drop",
            );
            const insertCommand = {
                insert: "coll",
                documents: [{_id: 1, value: 1}],
                lsid: retryableWriteSession.getSessionId(),
                txnNumber: NumberLong(1),
            };
            assert.commandWorked(retryableWriteSessionDB.runCommand(insertCommand));
            assert.commandWorked(retryableWriteSessionDB.runCommand(insertCommand));
            assert.eq({_id: 1, value: 1}, retryableWriteDB.coll.findOne({_id: 1}));
            retryableWriteSession.endSession();
        });
    }

    function runDropWithMismatchedLocalUUIDs() {
        const globalUUID = getSessionsCollection().uuid;
        for (const shardPrimary of getDataShardPrimaries()) {
            const shardConfigDB = shardPrimary.getDB("config");
            assert.commandWorked(shardConfigDB.runCommand({drop: "system.sessions"}));
            assert.commandWorked(shardConfigDB.runCommand({create: "system.sessions"}));
            const localUUID = shardConfigDB.getCollectionInfos({name: "system.sessions"})[0].info
                .uuid;
            assert.neq(globalUUID, localUUID, "failed to create a mismatched local UUID", {
                host: shardPrimary.host,
            });
        }
        assert.eq(globalUUID, getSessionsCollection().uuid);

        withSessionsCollectionDropped(function () {});
    }

    // The drop and the refresh-driven create both run their DDL coordinator on the first data
    // shard, so they serialize on the same collection DDL lock. The test below suspends the drop
    // coordinator while it holds the lock, runs the refresh concurrently, and verifies the end
    // state of both operations.
    function runDropThenRefreshCreateConcurrently() {
        setAllowDrop(true);
        const oldUUID = getSessionsCollection().uuid;

        const coordinator = extraShard.getPrimary();
        // Suspend the drop coordinator after it removed the collection from the global catalog;
        // it still holds the collection DDL lock while suspended.
        const dropFP = configureFailPoint(coordinator, "suspendDDLCoordinatorOnPhase", {
            operationType: "dropCollection_V2",
            nss: kSessionsNs,
            phase: "releaseCriticalSection",
        });
        const runCommand = (host, dbName, command) =>
            new Mongo(host).getDB(dbName).runCommand(command);
        const dropThread = new Thread(runCommand, st.s.host, "config", {drop: "system.sessions"});
        // Runs the same refresh path as the config primary's periodic refresh. MongoRunner
        // disables the automatic background refreshes.
        const refreshThread = new Thread(runCommand, st.configRS.getPrimary().host, "admin", {
            refreshLogicalSessionCacheNow: 1,
        });
        try {
            dropThread.start();
            dropFP.wait();
            assert.eq(null, st.s.getDB("config").collections.findOne({_id: kSessionsNs}));

            refreshThread.start();
            // Wait for the refresh-driven create to reach the shard holding the drop's DDL lock.
            assert.soon(
                () =>
                    coordinator
                        .getDB("admin")
                        .aggregate([
                            {$currentOp: {allUsers: true, idleConnections: false}},
                            {$match: {"command._shardsvrCreateCollection": "system.sessions"}},
                        ])
                        .itcount() > 0,
                "refresh-driven create did not reach the first shard",
            );
            // The refresh cannot create the collection while the suspended drop still holds
            // the collection DDL lock.
            assert.eq(null, st.s.getDB("config").collections.findOne({_id: kSessionsNs}));

            dropFP.off();
            assert.commandWorked(dropThread.returnData());
            assert.commandWorked(refreshThread.returnData());
            assert.neq(oldUUID, getSessionsCollection().uuid);
        } finally {
            dropFP.off();
            dropThread.returnData();
            refreshThread.returnData();
            refreshSessions();
        }
    }

    // -------------------------------------------------------------------------
    // One cluster for all tests.  Starts with a single shard (rs0).
    // -------------------------------------------------------------------------
    before(function () {
        st = new ShardingTest({
            shards: 1,
            mongos: 1,
        });
        st.stopBalancer();
        refreshSessions();
    });
    after(function () {
        setPreventSessionsCollectionSharding(false);
        try {
            setAllowDrop(false);
        } catch (e) {
            // Best-effort: the cluster may already be partially torn down.
        }
        if (extraShard) {
            stopExtraShard();
        }
        if (st) {
            st.stop();
            st = null;
        }
    });

    // -------------------------------------------------------------------------
    // Phase 1: single shard, toggle allowDropSystemSessionCollection at runtime.
    // -------------------------------------------------------------------------
    describe("on a single shard", function () {
        beforeEach(function () {
            dropTestDatabases();
            refreshSessions();
        });
        afterEach(function () {
            setAllowDrop(false);
        });

        it("rejects dropping config.system.sessions unless explicitly enabled", function () {
            setAllowDrop(false);
            assert.commandFailedWithCode(
                st.s.getDB("config").runCommand({drop: "system.sessions"}),
                ErrorCodes.IllegalOperation,
            );
            assertSessionsCollectionTracked();
        });

        it("rejects a collection UUID when dropping config.system.sessions", function () {
            setAllowDrop(true);
            assert.commandFailedWithCode(
                st.s.getDB("config").runCommand({
                    drop: "system.sessions",
                    collectionUUID: getSessionsCollection().uuid,
                }),
                ErrorCodes.InvalidOptions,
            );
            assertSessionsCollectionTracked();
        });

        it("still forbids dropping other config collections via mongos", function () {
            setAllowDrop(true);
            assert.commandFailedWithCode(
                st.s.getDB("config").runCommand({drop: "settings"}),
                ErrorCodes.IllegalOperation,
            );
        });

        it("supports CRUD after create and drop", function () {
            setAllowDrop(true);
            runCrudAfterDrop();
        });

        it("supports reads after create and drop", function () {
            setAllowDrop(true);
            runReadsAfterDrop();
        });

        it("supports transactions after create and drop", function () {
            setAllowDrop(true);
            runTransactionsAfterDrop();
        });

        it("supports retryable writes after create and drop", function () {
            setAllowDrop(true);
            runRetryableWritesAfterDrop();
        });

        it("removes sessions metadata from the config server shard catalog", function () {
            setAllowDrop(true);
            const sessionsUUID = getSessionsCollection().uuid;
            const configPrimary = st.configRS.getPrimary();
            const shardCatalogEntry = configPrimary
                .getDB("config")
                .getCollection("shard.catalog.collections")
                .findOne({_id: kSessionsNs});
            assert.neq(
                null,
                shardCatalogEntry,
                "config.system.sessions should be tracked in the config server's shard catalog",
            );
            assert.eq(sessionsUUID, shardCatalogEntry.uuid);
            dropSessionsCollection();
        });

        it("reports no metadata inconsistencies after drop", function () {
            setAllowDrop(true);
            withSessionsCollectionDropped(function () {
                const inconsistencies = st.s.getDB("admin").checkMetadataConsistency().toArray();
                assert.eq(0, inconsistencies.length, tojson(inconsistencies));
            });
        });

        it("drops sessions when a shard has a mismatched local UUID", function () {
            setAllowDrop(true);
            runDropWithMismatchedLocalUUIDs();
        });

        it("drops sessions that are missing from the global catalog", function () {
            setAllowDrop(true);
            const sessionsUUID = getSessionsCollection().uuid;

            for (const shardPrimary of getDataShardPrimaries()) {
                assert.gt(
                    shardPrimary.getDB("config").getCollectionInfos({name: "system.sessions"})
                        .length,
                    0,
                    "expected a local sessions collection before unregistering it",
                    {host: shardPrimary.host},
                );
            }

            // Unregister the collection from the configsvr, leaving local copies in place.
            setPreventSessionsCollectionSharding(true);
            try {
                const configDB = st.s.getDB("config");
                assert.commandWorked(
                    configDB
                        .getCollection("collections")
                        .deleteOne({_id: kSessionsNs}, {writeConcern: {w: "majority"}}),
                );
                assert.commandWorked(
                    configDB
                        .getCollection("chunks")
                        .deleteMany({uuid: sessionsUUID}, {writeConcern: {w: "majority"}}),
                );
                assert.eq(null, configDB.collections.findOne({_id: kSessionsNs}));

                // Drop via mongos should follow the untracked path and clean leftover local copies.
                assert.commandWorked(st.s.getDB("config").runCommand({drop: "system.sessions"}));

                // Verify local copies are gone and the collection stays untracked.
                assert.eq(null, configDB.collections.findOne({_id: kSessionsNs}));
                for (const shardPrimary of getDataShardPrimaries()) {
                    assertNoSessionsCollectionOnServer(shardPrimary);
                }
                assertNoSessionsCollectionOnServer(st.configRS.getPrimary());
            } finally {
                dropLocalSessionsLeftovers(sessionsUUID);
                setPreventSessionsCollectionSharding(false);
                refreshSessions();
            }
        });
    });

    // -------------------------------------------------------------------------
    // Phase 2: add a second shard named "aaaa" that sorts first alphabetically,
    // making it the first shard. The sessions collection stays on rs0 (now the
    // non-first shard). The shard is removed and re-added fresh for every test case.
    // -------------------------------------------------------------------------
    describe("after adding a new first shard while sessions remain on the original shard", function () {
        beforeEach(function () {
            dropTestDatabases();
            refreshSessions();
            setAllowDrop(false);
            startExtraShard();
            // Verify the sessions collection was not created on the new shard.
            assertNoSessionsCollectionOnServer(extraShard.getPrimary());
        });
        afterEach(function () {
            setAllowDrop(false);
            stopExtraShard();
        });

        it("supports CRUD after adding a non-first shard and dropping", function () {
            setAllowDrop(true);
            runCrudAfterDrop();
        });

        it("supports reads after adding a non-first shard and dropping", function () {
            setAllowDrop(true);
            runReadsAfterDrop();
        });

        it("supports transactions after adding a non-first shard and dropping", function () {
            setAllowDrop(true);
            runTransactionsAfterDrop();
        });

        it("supports retryable writes after adding a non-first shard and dropping", function () {
            setAllowDrop(true);
            runRetryableWritesAfterDrop();
        });

        it("drops sessions with mismatched local UUIDs on every shard", function () {
            setAllowDrop(true);
            runDropWithMismatchedLocalUUIDs();
        });

        it("serializes a refresh-driven create behind an in-progress mongos drop", function () {
            runDropThenRefreshCreateConcurrently();
        });

        it("reports no metadata inconsistencies after drop", function () {
            setAllowDrop(true);
            withSessionsCollectionDropped(function () {
                const inconsistencies = st.s.getDB("admin").checkMetadataConsistency().toArray();
                assert.eq(0, inconsistencies.length, tojson(inconsistencies));
            });
        });
    });

    // -------------------------------------------------------------------------
    // Phase 3: with "aaaa" as the first shard, move the sessions chunk FROM
    // "aaaa" (first shard) TO rs0 (non-first shard), leaving the first shard
    // empty.  Then drop the sessions collection that lives on rs0.
    // -------------------------------------------------------------------------
    describe("after migrating sessions to a non-first shard", function () {
        before(function () {
            setAllowDrop(false);
            startExtraShard();
        });
        after(function () {
            setAllowDrop(false);
            stopExtraShard();
        });
        beforeEach(function () {
            dropTestDatabases();
            setAllowDrop(true);
            // Ensure the sessions collection exists on the first shard ("aaaa")
            // by dropping it first, then refreshing so it gets recreated there.
            if (st.s.getDB("config").collections.findOne({_id: kSessionsNs}) !== null) {
                dropSessionsCollection();
            } else {
                refreshSessions();
            }
            // Move the sessions chunk FROM the first shard ("aaaa") TO rs0
            // (the non-first shard).
            assert.commandWorked(
                st.s.adminCommand({
                    moveChunk: kSessionsNs,
                    find: {_id: 0},
                    to: st.shard0.shardName,
                }),
            );
            const configDB = st.s.getDB("config");
            const sessions = configDB.collections.findOne({_id: kSessionsNs});
            assert.neq(null, sessions);
            const chunks = configDB.chunks.find({uuid: sessions.uuid}).toArray();
            jsTest.log.info("sessions chunks after moveChunk", {chunks});
            assert.eq(1, chunks.length, "expected exactly one sessions chunk", {chunks});
            assert.eq(
                st.shard0.shardName,
                chunks[0].shard,
                "sessions chunk should be on rs0 (non-first shard)",
                {chunks},
            );
        });

        it("supports CRUD after migrating sessions to a non-first shard and dropping", function () {
            setAllowDrop(true);
            runCrudAfterDrop();
        });

        it("supports reads after migrating sessions to a non-first shard and dropping", function () {
            setAllowDrop(true);
            runReadsAfterDrop();
        });

        it("supports transactions after migrating sessions to a non-first shard and dropping", function () {
            setAllowDrop(true);
            runTransactionsAfterDrop();
        });

        it("supports retryable writes after migrating sessions to a non-first shard and dropping", function () {
            setAllowDrop(true);
            runRetryableWritesAfterDrop();
        });

        it("drops sessions with mismatched local UUIDs after migration", function () {
            setAllowDrop(true);
            runDropWithMismatchedLocalUUIDs();
        });

        it("reports no metadata inconsistencies after drop", function () {
            setAllowDrop(true);
            withSessionsCollectionDropped(function () {
                const inconsistencies = st.s.getDB("admin").checkMetadataConsistency().toArray();
                assert.eq(0, inconsistencies.length, tojson(inconsistencies));
            });
        });
    });
});
