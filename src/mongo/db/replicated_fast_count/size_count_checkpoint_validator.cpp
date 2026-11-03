// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"

#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/db/repl/container_oplog_entry_serialization.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_metrics.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store_oplog.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <algorithm>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {

// Adds one byte to the size of each live collection in the standby's expected checkpoint.
MONGO_FAIL_POINT_DEFINE(skewStandbyCheckpointSize);

namespace {

thread_local std::vector<ValidatorDeltaRecord>* tValidatorBuffer = nullptr;

void report(ReportingStrategy strategy, int code, std::string message) {
    if (strategy == ReportingStrategy::kFassert) {
        fassertFailed(code, {ErrorCodes::InternalError, std::move(message)});
    } else {
        LOGV2_ERROR(
            code, "Replicated metadata checkpoint validation failed", "error"_attr = message);
    }
}

UUID uuidFromContainerInnerOp(const repl::DurableReplOperation& inner) {
    repl::ContainerKey key;
    switch (inner.getOpType()) {
        case repl::OpTypeEnum::kContainerInsert: {
            const auto parsed = repl::ContainerInsertOplogEntryO::parse(
                inner.getObject(), IDLParserContext("FastCountCheckpointInnerInsert"));
            key = parsed.getKey();
            break;
        }
        case repl::OpTypeEnum::kContainerUpdate: {
            const auto parsed = repl::ContainerUpdateOplogEntryO::parse(
                inner.getObject(), IDLParserContext("FastCountCheckpointInnerUpdate"));
            key = parsed.getKey();
            break;
        }
        case repl::OpTypeEnum::kContainerDelete: {
            const auto parsed = repl::ContainerDeleteOplogEntryO::parse(
                inner.getObject(), IDLParserContext("FastCountCheckpointInnerDelete"));
            key = parsed.getKey();
            break;
        }
        default:
            MONGO_UNREACHABLE;
    }
    const auto keyBytes = key.getBytesKey();
    return UUID::fromCDR(ConstDataRange(keyBytes.data(), keyBytes.data() + keyBytes.size()));
}

absl::flat_hash_map<UUID, CollectionReplicatedMetadata> scanMetadataStore(OperationContext* opCtx) {
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx);
    return writeConflictRetry(
        opCtx, ru, "scanMetadataStore", NamespaceString::kAdminCommandNamespace, [&] {
            Lock::GlobalLock readLock(opCtx, MODE_IS);
            auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
            invariant(storageEngine->getEngine()->hasIdent(ru, ident::kFastCountMetadataStore),
                      "Replicated fast count metadata store must exist to adopt a checkpoint");
            const auto recordStore = storageEngine->getEngine()->getRecordStore(
                opCtx,
                NamespaceString::kAdminCommandNamespace,
                ident::kFastCountMetadataStore,
                RecordStore::Options{.keyFormat = KeyFormat::String},
                /*uuid=*/boost::none);

            const auto& containerVariant = recordStore->getContainer();
            massert(13629502,
                    "Expected replicated fast count metadata record store to hold a "
                    "StringKeyedContainer",
                    std::holds_alternative<std::reference_wrapper<StringKeyedContainer>>(
                        containerVariant));
            auto& container =
                std::get<std::reference_wrapper<StringKeyedContainer>>(containerVariant).get();
            auto cursor = container.getCursor(ru);

            absl::flat_hash_map<UUID, CollectionReplicatedMetadata> contents;
            while (auto record = cursor->next()) {
                const auto& [keySpan, valueSpan] = *record;
                const auto uuid = UUID::fromCDR(ConstDataRange(keySpan.data(), keySpan.size()));
                const auto entry = SizeCountStore::parseContainerValue(valueSpan);
                contents[uuid] = CollectionReplicatedMetadata{
                    .sizeCount = CollectionSizeCount{.size = entry.size, .count = entry.count},
                    .hash = entry.hash};
            }
            return contents;
        });
}

/**
 * Adds the provided `delta` to the provided `entry`, accounting for DDL state.
 */
void addDelta(ReplicatedMetadataDelta& entry, const ReplicatedMetadataDelta& delta) {
    entry.metadata = entry.metadata + delta.metadata;
    switch (delta.state) {
        case DDLState::kNone:
            // `computeExpectedCheckpoint()` uses DDL state to know if an entry should exist in
            // `_baseline`. If a collection is created and written to in the same checkpoint
            // interval, its DDL state should stay `DDLState::kCreated` so that
            // `computeExpectedCheckpoint()` does not expect an entry in `_baseline`. Thus, we do
            // not overwrite pre-existing DDL state with `DDLState::kNone`.
            break;
        case DDLState::kCreated:
            invariant(entry.state != DDLState::kDropped,
                      "Checkpoint validation does not support collection re-creation with the "
                      "same UUID");
            entry.state = DDLState::kCreated;
            break;
        case DDLState::kDropped:
            entry.state = DDLState::kDropped;
            break;
        case DDLState::kDroppedAndRecreated:
            // TODO(SERVER-137910): After shard migration is enabled, a delta can have DDL state
            // `kDroppedAndRecreated`.
            MONGO_UNREACHABLE;
    }
}
}  // namespace

void validateCheckpoint(const CheckpointContents& standby,
                        const CheckpointContents& primary,
                        ReportingStrategy reportingStrategy) {
    for (const auto& [uuid, metadata] : primary) {
        // Check each entry in `primary` exists in `standby`, i.e., `primary` ⊆ `standby`.
        const auto standbyIt = standby.find(uuid);
        if (standbyIt == standby.end()) {
            if (metadata.has_value()) {
                report(reportingStrategy,
                       13626001,
                       fmt::format("Replicated metadata checkpoint contains a write for collection "
                                   "{} which the standby observed no changes for",
                                   uuid.toString()));
            } else {
                report(
                    reportingStrategy,
                    13626002,
                    fmt::format("Replicated metadata checkpoint is missing collection {} which the "
                                "standby did not observe as dropped",
                                uuid.toString()));
            }
            continue;
        }

        // Check that the collection was dropped in both maps or neither map, i.e., `primaryRemoved`
        // == `standbyRemoved`.
        const bool primaryRemoved = !metadata.has_value();
        const bool standbyRemoved = !standbyIt->second.has_value();
        if (primaryRemoved && !standbyRemoved) {
            report(reportingStrategy,
                   13626003,
                   fmt::format("Replicated metadata checkpoint drops collection {} "
                               "which the standby did not observe as dropped",
                               uuid.toString()));
        } else if (!primaryRemoved && standbyRemoved) {
            report(reportingStrategy,
                   13626004,
                   fmt::format("Replicated metadata checkpoint writes collection {} which the "
                               "standby observed as dropped",
                               uuid.toString()));
        } else if (!primaryRemoved && !standbyRemoved) {
            // Check that the metadata match if neither collection was dropped.
            if (metadata->sizeCount != standbyIt->second->sizeCount) {
                report(reportingStrategy,
                       13626005,
                       fmt::format("Replicated metadata checkpoint mismatch for collection {}: "
                                   "standby {{{}}}, primary checkpoint has {{{}}}",
                                   uuid.toString(),
                                   standbyIt->second->toString(),
                                   metadata->toString()));
            }
        }
    }

    // `primary` ⊆ `standby` ∧ |`standby`| == |`primary`| ⇒ `standby` == `primary`. This early
    // return is an optimization. If the sizes are not equal, we will discover the missing
    // entry or entries below.
    if (standby.size() == primary.size() && reportingStrategy == ReportingStrategy::kFassert) {
        return;
    }

    // Check each entry in `standby` exists in `primary`, i.e., `standby` ⊆ `primary`.
    for (const auto& [uuid, metadata] : standby) {
        if (!primary.contains(uuid)) {
            report(reportingStrategy,
                   13626006,
                   fmt::format("Replicated metadata checkpoint is missing a write for collection "
                               "{} which the standby observed changed (state: {})",
                               uuid.toString(),
                               metadata ? "active" : "dropped"));
        }
    }
}

bool isCheckpointApplyOpsEntry(const repl::OplogEntry& entry) {
    if (entry.getOpType() != repl::OpTypeEnum::kCommand ||
        entry.getCommandType() != repl::CommandTypeEnum::kApplyOps) {
        return false;
    }
    const auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(entry.getObject());
    const auto& ops = applyOpsInfo.getOperations();
    if (ops.empty()) {
        return false;
    }

    // Check that the first applyOps entry is a container write to either the metadata store or the
    // timestamp store. If not, this entry is not a checkpoint.
    const auto firstOp = repl::DurableReplOperation::parse(
        ops.front(), IDLParserContext("FastCountCheckpointInnerFirstOp"));
    if (!firstOp.getContainer() ||
        (*firstOp.getContainer() != ident::kFastCountMetadataStore &&
         *firstOp.getContainer() != ident::kFastCountMetadataStoreTimestamps)) {
        return false;
    }

    // Check that the checkpoint contains at least one container write to both the metadata
    // store and the timestamps store. The checkpoint (one metadata store write per updated
    // collection and one timestamp store write) may be split across multiple applyOps entries if
    // the size of the entry exceeds maxSizeOfBatchedOperationsInSingleOplogEntryBytes (default
    // 16MB) OR if maxNumberOfBatchedOperationsInSingleOplogEntry is set to a value <  the # of
    // metadata store writes + 1 (the timestamp store write).
    // TODO(SERVER-136501): Validate checkpoints with applyOps entries exceeding 16MB.
    bool hasMetadataStoreWrite = false;
    bool hasTimestampStoreWrite = false;
    for (const auto& innerOpObj : ops) {
        const auto innerOp = repl::DurableReplOperation::parse(innerOpObj);
        tassert(13722400,
                "Fast count checkpoint applyOps entry contains a non-container operation",
                innerOp.getContainer().has_value());

        if (*innerOp.getContainer() == ident::kFastCountMetadataStore) {
            hasMetadataStoreWrite = true;
        } else if (*innerOp.getContainer() == ident::kFastCountMetadataStoreTimestamps) {
            tassert(13647901,
                    "No container deletes are applied to the timestamp container",
                    innerOp.getOpType() != repl::OpTypeEnum::kContainerDelete);
            hasTimestampStoreWrite = true;
        }
    }
    return hasMetadataStoreWrite && hasTimestampStoreWrite;
}

CheckpointContents parseCheckpoint(const repl::OplogEntry& entry) {
    CheckpointContents contents;
    const auto applyOpsInfo = repl::ApplyOpsCommandInfo::parse(entry.getObject());
    for (const auto& innerOpObj : applyOpsInfo.getOperations()) {
        const auto innerOp = repl::DurableReplOperation::parse(innerOpObj);
        if (!innerOp.getContainer() || *innerOp.getContainer() != ident::kFastCountMetadataStore) {
            continue;
        }
        switch (innerOp.getOpType()) {
            case repl::OpTypeEnum::kContainerDelete: {
                // A container delete entry does not include any replicated metadata, so we set the
                // value to boost::none.
                contents[uuidFromContainerInnerOp(innerOp)] = boost::none;
                break;
            }
            case repl::OpTypeEnum::kContainerInsert: {
                const auto parsed = repl::ContainerInsertOplogEntryO::parse(
                    innerOp.getObject(), IDLParserContext("FastCountCheckpointInnerInsert"));
                massert(13647900,
                        "Fast count metadata store container write is missing a value",
                        parsed.getValue().has_value());
                const BSONObj valueBson = BSONObj(parsed.getValue()->data().data()).getOwned();
                const SizeCountStore::Entry storeEntry = SizeCountStore::parseContainerValue(
                    {valueBson.objdata(), static_cast<size_t>(valueBson.objsize())});
                contents[uuidFromContainerInnerOp(innerOp)] = CollectionReplicatedMetadata{
                    .sizeCount =
                        CollectionSizeCount{.size = storeEntry.size, .count = storeEntry.count},
                    .hash = storeEntry.hash};
                break;
            }
            case repl::OpTypeEnum::kContainerUpdate: {
                const auto parsed = repl::ContainerUpdateOplogEntryO::parse(
                    innerOp.getObject(), IDLParserContext("FastCountCheckpointInnerUpdate"));
                const BSONObj valueBson = BSONObj(parsed.getValue().data().data()).getOwned();
                const SizeCountStore::Entry storeEntry = SizeCountStore::parseContainerValue(
                    {valueBson.objdata(), static_cast<size_t>(valueBson.objsize())});
                contents[uuidFromContainerInnerOp(innerOp)] = CollectionReplicatedMetadata{
                    .sizeCount =
                        CollectionSizeCount{.size = storeEntry.size, .count = storeEntry.count},
                    .hash = storeEntry.hash};
                break;
            }
            default:
                MONGO_UNREACHABLE;
        }
    }
    return contents;
}

void SizeCountCheckpointValidator::preprocessOplogEntry(const repl::OplogEntry& entry) {
    if (entry.getOpType() == repl::OpTypeEnum::kNoop) {
        if (entry.getObject().getStringField("msg") == kWatermarkMsg) {
            recordWatermark(entry.getOpTime().getTimestamp());
        }
        return;
    }
    if (isCheckpointApplyOpsEntry(entry)) {
        recordCheckpoint(entry);
    }
}

void SizeCountCheckpointValidator::recordWatermark(Timestamp watermarkTs) {
    // Watermark timestamps are monotonically increasing, so `watermarkTs` should be the largest
    // timestamp seen so far. This also guarantees the queue is sorted.
    invariant(_intervals.empty() || watermarkTs > _intervals.back().cutoff,
              fmt::format("Watermark timestamps should be monotonically increasing; "
                          "watermarkTs={}, cutoff={}",
                          watermarkTs,
                          _intervals.back().cutoff));
    _intervals.push_back(ValidatorInterval{.cutoff = watermarkTs, .deltas = std::move(_ongoing)});
    _ongoing = ReplicatedMetadataDeltas{};
    incrementWatermarksRecordedCount();
}

void SizeCountCheckpointValidator::recordCheckpoint(const repl::OplogEntry& entry) {
    // This copy is cheap because the copy constructor of the BSONObj inside the OplogEntry just
    // increments the reference count of a SharedBuffer.
    _checkpoints.push_back(entry);
}

void SizeCountCheckpointValidator::recordDelta(UUID uuid,
                                               Timestamp ts,
                                               ReplicatedMetadataDelta delta) {
    if (!tValidatorBuffer) {
        return;
    }
    tValidatorBuffer->push_back(
        ValidatorDeltaRecord{.ts = ts, .uuid = uuid, .delta = std::move(delta)});
}

SizeCountCheckpointValidator::ScopedThreadBuffer::ScopedThreadBuffer(
    std::vector<ValidatorDeltaRecord>* buffer) {
    tValidatorBuffer = buffer;
}

SizeCountCheckpointValidator::ScopedThreadBuffer::~ScopedThreadBuffer() {
    tValidatorBuffer = nullptr;
}

void SizeCountCheckpointValidator::mergeAndValidate(OperationContext* opCtx,
                                                    const ValidatorBuffers& threadRecords,
                                                    UUID oplogUuid) {
    merge(threadRecords);

    if (!_baseline.has_value()) {
        adoptCheckpoint(opCtx);
        return;
    }

    for (const auto& entry : _checkpoints) {
        const Timestamp validAsOf =
            getTimestampStoreValidAsOfFromOplogEntry(entry.getEntry().toBSON());

        // Find the interval with the checkpoint's validAsOf timestamp.
        auto intervalIt = std::find_if(
            _intervals.begin(), _intervals.end(), [&](const ValidatorInterval& interval) {
                return interval.cutoff == validAsOf;
            });
        invariant(intervalIt != _intervals.end(),
                  fmt::format("Replicated metadata checkpoint's validAsOf timestamp {} does not "
                              "match any recorded watermark after the baseline was initialized",
                              validAsOf));

        const CheckpointContents standby = computeExpectedCheckpoint(validAsOf);
        CheckpointContents primary = parseCheckpoint(entry);
        // The primary's checkpoint includes replicated metadata for the oplog, but the standby's
        // checkpoint does not. This is because the primary _derives_ oplog replicated metadata
        // while tailing the oplog; the oplog's replicated metadata is not explicitly replicated
        // through the m (metadata) field. Remove the entry from primary's checkpoint so we do not
        // validate it.
        primary.erase(oplogUuid);

        if (repl::enableContinuousInternodePerCollectionValidation.load()) {
            validateCheckpoint(standby, primary, ReportingStrategy::kFassert);
        } else {
            validateCheckpoint(standby, primary, ReportingStrategy::kLogError);
        }
        incrementCheckpointsValidatedCount();

        // After validating the checkpoint, we consider all intervals with cutoff <= validAsOf
        // validated. This includes intervals/watermarks that do not have an associated checkpoint.
        // TODO(SERVER-135085): This loop would go away if there existed one watermark per
        // checkpoint. We would simply erase the interval once.
        while (!_intervals.empty() && _intervals.front().cutoff <= validAsOf) {
            _intervals.pop_front();
            incrementWatermarksEvictedCount();
        }

        LOGV2(13380115,
              "Checkpoint validator validated checkpoint",
              "validAsOf"_attr = validAsOf,
              "validatedCollections"_attr = standby.size());
    }

    _checkpoints.clear();
}

void SizeCountCheckpointValidator::merge(const ValidatorBuffers& threadRecords) {
    for (const auto& records : threadRecords) {
        for (const auto& rec : records) {
            // Find the first interval with cutoff >= ts. After folding, each interval contains the
            // replicated metadata deltas between the previous interval cutoff (exclusive) and its
            // own cutoff (inclusive).
            auto it = std::find_if(
                _intervals.begin(), _intervals.end(), [&](const ValidatorInterval& interval) {
                    return interval.cutoff >= rec.ts;
                });
            if (it != _intervals.end()) {
                invariant(
                    it->cutoff != rec.ts,
                    "Timestamp of a replicated metadata delta should never equal the timestamp "
                    "of a watermark");
                addDelta(it->deltas[rec.uuid], rec.delta);
            } else {
                // The watermark will arrive in another batch, so we fold this record into the
                // _ongoing map.
                addDelta(_ongoing[rec.uuid], rec.delta);
            }
        }
    }
}

void SizeCountCheckpointValidator::adoptCheckpoint(OperationContext* opCtx) {
    if (_checkpoints.empty()) {
        return;
    }

    // Since `_checkpoints` is sorted in increasing timestamp order, the last checkpoint is also the
    // latest checkpoint.
    const auto& latestCheckpoint = _checkpoints.back();
    const Timestamp validAsOf =
        getTimestampStoreValidAsOfFromOplogEntry(latestCheckpoint.getEntry().toBSON());

    // To adopt a checkpoint, we must see its corresponding watermark first. Assume, for the sake of
    // contradiction, that we can adopt a checkpoint without seeing its watermark. Then, given the
    // following oplog with a watermark at timestamp T and a fault at timestamp T':
    //
    // ...--[W]----[deltas]----X---------[ckpt]---...
    //    ts = T            ts = T'   validAsOf = T
    //
    // When the standby restarts, starts applying oplog at T', and adopts ckpt with validAsOf T, it
    // misses all deltas ∈ (T, T'), and subsequent validation is unsound.
    //
    // If we instead only adopt a checkpoint after seeing its watermark at timestamp T, then we must
    // have recorded all deltas after T, and validation is sound.
    //
    // ...---X----[W]----[deltas]----[ckpt]---...
    //          ts = T            validAsOf = T
    //
    const bool watermarkExistsForValidAsOf =
        std::any_of(_intervals.begin(), _intervals.end(), [&](const ValidatorInterval& interval) {
            return interval.cutoff == validAsOf;
        });
    if (watermarkExistsForValidAsOf) {
        // `adoptCheckpoint()` runs after the batch has been applied, so the latest checkpoint's
        // container writes are already persisted in the metadata store. To initialize `_baseline`,
        // we scan the metadata store and adopt its contents without performing validation. From
        // this point on, we can validate all checkpoints by combining `_baseline` with new deltas.
        _baseline = scanMetadataStore(opCtx);
        while (!_intervals.empty() && _intervals.front().cutoff <= validAsOf) {
            _intervals.pop_front();
            incrementWatermarksEvictedCount();
        }
        LOGV2(13629501, "Checkpoint validator adopted checkpoint", "validAsOf"_attr = validAsOf);
    }

    incrementCheckpointsSkippedCount(_checkpoints.size());
    LOGV2(13629500,
          "Checkpoint validator processed checkpoints without performing validation",
          "checkpointsSkipped"_attr = _checkpoints.size());

    _checkpoints.clear();
}

CheckpointContents SizeCountCheckpointValidator::computeExpectedCheckpoint(Timestamp validAsOf) {
    invariant(_baseline.has_value(),
              "Expected checkpoint computation requires an initialized baseline");

    // Compute the sum of intervals with cutoff <= validAsOf timestamp.
    ReplicatedMetadataDeltas observed;
    auto it = _intervals.begin();
    while (it != _intervals.end() && it->cutoff <= validAsOf) {
        for (const auto& [uuid, delta] : it->deltas) {
            addDelta(observed[uuid], delta);
        }
        ++it;
    }
    invariant(it != _intervals.begin() && std::prev(it)->cutoff == validAsOf,
              "Expected checkpoint must end on the interval whose cutoff equals the checkpoint's "
              "validAsOf timestamp");

    // Combine the observed deltas with the baseline, advancing the baseline to reflect the
    // validated window. The baseline may contain entries that have not changed since the last
    // checkpoint, so `expected` only contains UUIDs in `observed`.
    CheckpointContents expected;
    for (const auto& [uuid, delta] : observed) {
        if (delta.state == DDLState::kDropped) {
            // If `_baseline` contains an entry for this UUID, it must have been created in another
            // checkpoint, so we expect a `boost::none` entry in the checkpoint.
            if (_baseline->contains(uuid)) {
                expected.emplace(uuid, boost::none);
            }
            // Otherwise, it might have been created and dropped within the same checkpoint, in
            // which case the entry would be absent from the primary's checkpoint. We do not add it
            // to `expected`.
            // TODO(SERVER-138264): Differentiate between a create and drop in one checkpoint vs. a
            // drop without a create.
            _baseline->erase(uuid);
            continue;
        }
        const CollectionReplicatedMetadata base = [&]() -> CollectionReplicatedMetadata {
            if (delta.state == DDLState::kCreated) {
                return CollectionReplicatedMetadata{.sizeCount =
                                                        CollectionSizeCount{.size = 0, .count = 0},
                                                    .hash = kEmptyCollectionValidationHash};
            }
            auto baseIt = _baseline->find(uuid);
            // TODO(SERVER-138205): Invariant that the UUID exists in the baseline once the
            // baseline is seeded with the primary's first checkpoint.
            return baseIt == _baseline->end()
                ? CollectionReplicatedMetadata{.sizeCount =
                                                   CollectionSizeCount{.size = 0, .count = 0}}
                : baseIt->second;
        }();
        const CollectionReplicatedMetadata absolute = base + delta.metadata;
        expected[uuid] = absolute;
        (*_baseline)[uuid] = absolute;
    }

    if (MONGO_unlikely(skewStandbyCheckpointSize.shouldFail())) {
        for (auto&& [uuid, metadata] : expected) {
            if (metadata) {
                metadata->sizeCount.size += 1;
            }
        }
    }

    return expected;
}

}  // namespace mongo::replicated_fast_count
