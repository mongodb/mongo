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
#include "mongo/db/storage/ident.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <algorithm>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {

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

void foldRecord(ReplicatedMetadataDeltas& deltas, const ValidatorDeltaRecord& rec) {
    // TODO(SERVER-136545): Remove.
    invariant(rec.delta.state == DDLState::kNone,
              "Checkpoint validation does not support DDL deltas yet");
    auto& entry = deltas[rec.uuid];
    entry.metadata = entry.metadata + rec.delta.metadata;
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
    const auto firstOp = repl::DurableReplOperation::parse(
        ops.front(), IDLParserContext("FastCountCheckpointInnerFirstOp"));
    return firstOp.getContainer() &&
        (*firstOp.getContainer() == ident::kFastCountMetadataStore ||
         *firstOp.getContainer() == ident::kFastCountMetadataStoreTimestamps);
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

void SizeCountCheckpointValidator::mergeAndValidate(const ValidatorBuffers& threadRecords) {
    merge(threadRecords);

    for (const auto& entry : _checkpoints) {
        const Timestamp validAsOf =
            getTimestampStoreValidAsOfFromOplogEntry(entry.getEntry().toBSON());

        // Find the interval with the checkpoint's validAsOf timestamp.
        auto intervalIt = std::find_if(
            _intervals.begin(), _intervals.end(), [&](const ValidatorInterval& interval) {
                return interval.cutoff == validAsOf;
            });
        if (intervalIt == _intervals.end()) {
            // TODO(SERVER-136295): Add a log and potentially a metric here. This should happen
            // exactly once per node lifetime, after recovery.

            // The watermark for this checkpoint was not recorded, so we cannot validate this
            // checkpoint.
            continue;
        }

        const CheckpointContents standby = computeExpectedCheckpoint(validAsOf);
        const CheckpointContents primary = parseCheckpoint(entry);

        // TODO(SERVER-136296): Validate with logging when the server parameter is off. We don't
        // validate/log for now because it will be noisy.
        if (repl::enableContinuousInternodePerCollectionValidation.load()) {
            validateCheckpoint(standby, primary, ReportingStrategy::kFassert);
            incrementCheckpointsValidatedCount();
        }

        // After validating the checkpoint, we consider all intervals with cutoff <= validAsOf
        // validated. This includes intervals/watermarks that do not have an associated checkpoint.
        // TODO(SERVER-135085): This loop would go away if there existed one watermark per
        // checkpoint. We would simply advance the baseline and erase the interval once.
        while (!_intervals.empty() && _intervals.front().cutoff <= validAsOf) {
            advanceBaseline(_intervals.front().deltas);
            // Evict the interval from our queue, even if we didn't see an exact checkpoint.
            _intervals.pop_front();
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
                foldRecord(it->deltas, rec);
            } else {
                // The watermark will arrive in another batch, so we fold this record into the
                // _ongoing map.
                foldRecord(_ongoing, rec);
            }
        }
    }
}

CheckpointContents SizeCountCheckpointValidator::computeExpectedCheckpoint(Timestamp validAsOf) {
    // Compute the sum of intervals with cutoff <= validAsOf timestamp.
    ReplicatedMetadataDeltas observed;
    auto it = _intervals.begin();
    while (it != _intervals.end() && it->cutoff <= validAsOf) {
        for (const auto& [uuid, delta] : it->deltas) {
            auto& merged = observed[uuid];
            merged.metadata = merged.metadata + delta.metadata;
        }
        ++it;
    }
    invariant(it != _intervals.begin() && std::prev(it)->cutoff == validAsOf,
              "Expected checkpoint must end on the interval whose cutoff equals the checkpoint's "
              "validAsOf timestamp");

    // Combine the observed deltas with the baseline. The baseline may contain entries that have not
    // changed since the last checkpoint, so we only want to return UUIDs in `observed`.
    CheckpointContents expected;
    for (const auto& [uuid, delta] : observed) {
        auto baseIt = _baseline.find(uuid);
        if (baseIt != _baseline.end()) {
            expected[uuid] = baseIt->second + delta.metadata;
        } else {
            expected[uuid] = delta.metadata;
        }
    }

    return expected;
}

void SizeCountCheckpointValidator::advanceBaseline(const ReplicatedMetadataDeltas& deltas) {
    // TODO(SERVER-136545): Combine this functionality with computeExpectedCheckpoint() or explain
    // why it is not possible.
    for (const auto& [uuid, delta] : deltas) {
        auto base = _baseline.find(uuid);
        if (base != _baseline.end()) {
            base->second = base->second + delta.metadata;
        } else {
            _baseline[uuid] = delta.metadata;
        }
    }
}

}  // namespace mongo::replicated_fast_count
