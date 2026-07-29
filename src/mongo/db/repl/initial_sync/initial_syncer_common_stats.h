// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace repl {
namespace initial_sync_common_stats {

enum class InitialSyncKind { kLogical, kFCBIS };
[[nodiscard]] static constexpr std::string_view initialSyncKindToStringView(InitialSyncKind kind) {
    switch (kind) {
        case InitialSyncKind::kLogical:
            return "logical"sv;
        case InitialSyncKind::kFCBIS:
            return "FCBIS"sv;
    }
    MONGO_UNREACHABLE;
}

void incrementInitialSyncFailedAttemptMetric(InitialSyncKind kind);
void incrementInitialSyncFailureMetric(InitialSyncKind kind);
void incrementInitialSyncCompleteMetric(InitialSyncKind kind);

[[nodiscard]] size_t getInitialSyncFailedAttemptCount();
[[nodiscard]] size_t getInitialSyncFailureCount();
[[nodiscard]] size_t getInitialSyncCompleteCount();

void LogInitialSyncAttemptStats(const StatusWith<OpTimeAndWallTime>& attemptResult,
                                bool hasRetries,
                                const BSONObj& stats);

}  // namespace initial_sync_common_stats
}  // namespace repl
}  // namespace mongo
