// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"

#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/db/repl/container_oplog_entry_serialization.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/storage/ident.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo::replicated_fast_count {

namespace {

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
    for (const auto& innerOpObj : applyOpsInfo.getOperations()) {
        const auto innerOp = repl::DurableReplOperation::parse(innerOpObj);
        const bool isContainerWrite = innerOp.getOpType() == repl::OpTypeEnum::kContainerInsert ||
            innerOp.getOpType() == repl::OpTypeEnum::kContainerUpdate ||
            innerOp.getOpType() == repl::OpTypeEnum::kContainerDelete;
        // A timestamp store write is included in every checkpoint, so we check for the existence of
        // this ident.
        if (isContainerWrite && innerOp.getContainer() &&
            *innerOp.getContainer() == ident::kFastCountMetadataStoreTimestamps) {
            tassert(13647901,
                    "No container deletes are applied to the timestamp container",
                    innerOp.getOpType() != repl::OpTypeEnum::kContainerDelete);
            return true;
        }
    }
    return false;
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

}  // namespace mongo::replicated_fast_count
