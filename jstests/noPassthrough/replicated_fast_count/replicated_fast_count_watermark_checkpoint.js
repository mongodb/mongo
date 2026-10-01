/**
 * Tests that a replicated fast count checkpoint is written with the valid-as-of timestamp of a
 * preceding no-op watermark entry.
 *
 * @tags: [
 *   requires_replication,
 *   requires_persistence,
 * ]
 */
import {FeatureFlagUtil} from "jstests/libs/feature_flag_util.js";
import {PersistenceProviderUtil} from "jstests/libs/server-rss/persistence_provider_util.js";
import {after, before, describe, it} from "jstests/libs/mochalite.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";

const kWatermarkMsg = "replicated collection metadata watermark";
const kTimestampsStoreIdent = "internal-fastCountMetadataStoreTimestamps";
const kMetadataStoreIdent = "internal-fastCountMetadataStore";
const kValidAsOfKey = "valid-as-of";

// Bound for time between watermark and checkpoint oplog entries.
const kPairTimeoutMs = 2 * 60 * 1000;

/**
 * Returns the query matching oplog entries that write to the container with `ident`. A container
 * write reaches the oplog either as a top-level container insert ('ci') or update ('cu'), or as a
 * single operation nested inside an applyOps command entry.
 */
function containerWriteQuery(ident) {
    return {
        $or: [
            {op: {$in: ["ci", "cu"]}, container: ident},
            {op: "c", ns: "admin.$cmd", "o.applyOps": {$elemMatch: {container: ident}}},
        ],
    };
}

/**
 * Returns the valid-as-of timestamp carried in the persisted container value of a container write
 * oplog entry targeting `ident`. The value is stored as BinData in the op's 'o.v' field.
 */
function getValidAsOfFromContainerWrite(entry, ident) {
    if (entry.container === ident) {
        return hexToBSON(entry.o.v.hex())[kValidAsOfKey];
    }
    const innerOp = entry.o.applyOps.find((op) => op.container === ident);
    return hexToBSON(innerOp.o.v.hex())[kValidAsOfKey];
}

describe("fast count watermark checkpoint", function () {
    before(function () {
        this.rst = new ReplSetTest({
            nodes: 1,
            nodeOptions: {
                syncdelay: 5, // Checkpoint every 5 seconds to satisfy the assert.soon() sooner.
            },
        });
        this.rst.startSet();
        this.rst.initiate();
        this.primary = this.rst.getPrimary();
        this.db = this.primary.getDB(jsTestName());
        this.oplog = this.primary.getDB("local").getCollection("oplog.rs");

        // TODO SERVER-117454: Remove this explicit check and use the featureFlagReplicatedFastCount
        // tag once the flag is enabled in all feature flag variants.
        if (
            PersistenceProviderUtil.allNodesHavePropertyWithValue(
                this.db,
                "shouldUseReplicatedFastCount",
                false,
            ) &&
            !FeatureFlagUtil.isEnabled(this.db, "ReplicatedFastCount")
        ) {
            this.rst.stopSet();
            quit();
        }

        // Create a collection and insert data so the flushed checkpoint has size and count deltas
        // for a user collection, on top of the oplog collection's own delta.
        assert.commandWorked(this.db.createCollection("coll"));
        assert.commandWorked(this.db.coll.insertMany([{x: 1}, {x: 2}]));
    });

    after(function () {
        this.rst.stopSet();
    });

    it("persists a checkpoint whose valid-as-of matches the watermark entry", function () {
        const lastState = {};
        assert.soon(
            () => {
                const watermark = this.oplog
                    .find({op: "n", "o.msg": kWatermarkMsg})
                    .sort({ts: -1})
                    .limit(1)
                    .toArray()[0];
                const timestampStoreWrite = this.oplog
                    .find(containerWriteQuery(kTimestampsStoreIdent))
                    .sort({ts: -1})
                    .limit(1)
                    .toArray()[0];

                if (!watermark || !timestampStoreWrite) {
                    Object.assign(lastState, {watermark, checkpointWrite: timestampStoreWrite});
                    return false;
                }

                const validAsOf = getValidAsOfFromContainerWrite(
                    timestampStoreWrite,
                    kTimestampsStoreIdent,
                );

                // Find the latest container write to the metadata store with the timestamp >= the
                // watermark entry timestamp.
                const metadataWrites = this.oplog
                    .find(containerWriteQuery(kMetadataStoreIdent))
                    .sort({ts: 1})
                    .toArray()
                    .filter((e) => timestampCmp(e.ts, watermark.ts) >= 0);

                Object.assign(lastState, {
                    watermark,
                    checkpointWrite: timestampStoreWrite,
                    validAsOf,
                    metadataWrites,
                });

                return (
                    timestampCmp(validAsOf, watermark.ts) === 0 &&
                    // The checkpoint is committed after the watermark that cut its batch.
                    timestampCmp(timestampStoreWrite.ts, watermark.ts) > 0 &&
                    // The valid-as-of timestamp in each container write to the metadata store
                    // should be the same as the watermark entry.
                    metadataWrites.every(
                        (e) =>
                            timestampCmp(
                                getValidAsOfFromContainerWrite(e, kMetadataStoreIdent),
                                watermark.ts,
                            ) === 0,
                    )
                );
            },
            () => "Expected a watermark entry with a matching timestamp store checkpoint write",
            kPairTimeoutMs,
            undefined,
            undefined,
            lastState,
        );

        const metrics = this.db.serverStatus().metrics.replicatedFastCount;
        assert.gt(metrics.watermarksWritten, 0, "Expected watermarksWritten > 0", {metrics});
        assert.gt(metrics.tailer.watermarksSeen, 0, "Expected tailer.watermarksSeen > 0", {
            metrics,
        });
    });
});
