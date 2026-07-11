// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/logical_session_cache.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * A noop logical session cache for use in tests
 */
class [[MONGO_MOD_PUBLIC]] LogicalSessionCacheNoop : public LogicalSessionCache {
public:
    void joinOnShutDown() override {}

    Status startSession(OperationContext* opCtx, const LogicalSessionRecord& record) override {
        return Status::OK();
    }

    Status vivify(OperationContext* opCtx, const LogicalSessionId& lsid) override {
        return Status::OK();
    }

    Status refreshNow(OperationContext* opCtx) override {
        return Status::OK();
    }

    void reapNow(OperationContext* opCtx) override {}

    size_t size() override {
        return 0;
    }

    std::vector<LogicalSessionId> listIds() const override {
        return {};
    }

    std::vector<LogicalSessionId> listIds(
        const std::vector<SHA256Block>& userDigest) const override {
        return {};
    }

    boost::optional<LogicalSessionRecord> peekCached(const LogicalSessionId& id) const override {
        return boost::none;
    }

    LogicalSessionCacheStats getStats() override {
        return {};
    };

    void endSessions(const LogicalSessionIdSet& lsids) override {}
};

}  // namespace mongo
