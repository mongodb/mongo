/**
 * Tests basic functionality of the repairReplicatedMetadata command.
 *
 * @tags: [
 *   requires_replication,
 *   requires_sharding,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

// Standalone mongod: the command requires replication.
{
    const standalone = MongoRunner.runMongod({});
    assert(standalone);

    const usesReplicatedFastCount = PersistenceProviderUtil.allNodesHavePropertyWithValue(
        standalone.getDB("admin"),
        "shouldUseReplicatedFastCount",
        true,
        [standalone],
    );
    if (!usesReplicatedFastCount) {
        assert.commandFailedWithCode(
            standalone
                .getDB("admin")
                .runCommand({repairReplicatedMetadata: 1, uuid: UUID(), metadata: {}}),
            ErrorCodes.NoReplicationEnabled,
        );
    }

    MongoRunner.stopMongod(standalone);
}

// Replica set: the command runs on the primary.
{
    const rst = new ReplSetTest({nodes: 2});
    rst.startSet();
    rst.initiate();

    const primary = rst.getPrimary();
    const db = primary.getDB(jsTestName());
    assert.commandWorked(db.createCollection("t"));
    const uuid = db.getCollectionInfos({name: "t"})[0].info.uuid;

    // 'uuid' and 'metadata' are required.
    assert.commandFailedWithCode(
        primary.getDB("admin").runCommand({repairReplicatedMetadata: 1}),
        ErrorCodes.IDLFailedToParse,
    );

    const res = assert.commandWorked(
        primary.getDB("admin").runCommand({
            repairReplicatedMetadata: 1,
            uuid: uuid,
            metadata: {sz: 100, ct: 5},
            writeConcern: {w: "majority"},
        }),
    );
    assert.eq(res.writeConcernError, undefined);

    const entries = primary
        .getDB("local")
        .oplog.rs.find({op: "n", "o2.type": "repairReplicatedMetadata"})
        .toArray();
    assert.eq(entries.length, 1);
    assert.eq(entries[0].o.msg, "Repairing collection's replicated metadata with diffs");
    assert.eq(entries[0].ui, uuid);
    assert.eq(entries[0].o2.uuid, uuid);
    assert.eq(entries[0].o2.m.sz, 100);
    assert.eq(entries[0].o2.m.ct, 5);
    // Omitted subfields are not repaired, so a size/count repair must not carry a hash diff.
    assert(!entries[0].o2.m.hasOwnProperty("h"));

    // 'h' carries a collection validation hash diff.
    const hash = NumberLong("81985529216486895"); // 0x0123456789abcdef
    assert.commandWorked(
        primary.getDB("admin").runCommand({
            repairReplicatedMetadata: 1,
            uuid: uuid,
            metadata: {h: hash},
            writeConcern: {w: "majority"},
        }),
    );
    const hashEntries = primary
        .getDB("local")
        .oplog.rs.find({op: "n", "o2.type": "repairReplicatedMetadata", "o2.m.h": {$exists: true}})
        .toArray();
    assert.eq(hashEntries.length, 1);
    assert.eq(hashEntries[0].o2.m.h, hash);
    // A hash-only repair must not carry size/count diffs.
    assert(!hashEntries[0].o2.m.hasOwnProperty("sz"));
    assert(!hashEntries[0].o2.m.hasOwnProperty("ct"));

    // A collection that does not exist is a true no-op and writes no oplog entry.
    assert.commandWorked(
        primary
            .getDB("admin")
            .runCommand({repairReplicatedMetadata: 1, uuid: UUID(), metadata: {sz: 100, h: hash}}),
    );
    assert.eq(
        primary
            .getDB("local")
            .oplog.rs.find({op: "n", "o2.type": "repairReplicatedMetadata"})
            .toArray().length,
        2,
    );

    assert.commandFailedWithCode(
        rst
            .getSecondary()
            .getDB("admin")
            .runCommand({repairReplicatedMetadata: 1, uuid: uuid, metadata: {}}),
        ErrorCodes.NotWritablePrimary,
    );

    rst.awaitReplication();

    // The repair updates the in-memory size/count on the primary and, once replicated, on the
    // secondary.
    const secondary = rst.getSecondary();
    const usesReplicatedFastCount =
        PersistenceProviderUtil.allNodesHavePropertyWithValue(
            primary.getDB("admin"),
            "shouldUseReplicatedFastCount",
            true,
            [primary, secondary],
        ) || FeatureFlagUtil.isEnabled(db, "ReplicatedFastCount");
    if (usesReplicatedFastCount) {
        assert.eq(db.t.stats().count, 5);
        assert.eq(db.t.stats().size, 100);

        secondary.getDB(jsTestName()).getMongo().setSecondaryOk();
        const secondaryDb = secondary.getDB(jsTestName());
        assert.eq(secondaryDb.t.stats().count, 5);
        assert.eq(secondaryDb.t.stats().size, 100);
    }

    rst.stopSet();
}

// Sharded cluster: the command is not registered on mongos.
{
    const st = new ShardingTest({shards: 1, rs: {nodes: 1}});

    assert.commandFailedWithCode(
        st.s.getDB("admin").runCommand({repairReplicatedMetadata: 1}),
        ErrorCodes.CommandNotFound,
    );

    st.stop();
}
