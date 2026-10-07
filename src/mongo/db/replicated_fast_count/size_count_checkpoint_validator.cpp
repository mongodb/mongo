// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"

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

}  // namespace mongo::replicated_fast_count
