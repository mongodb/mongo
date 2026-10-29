// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * Tracks the number of external clients on the shard port of a mongod.
 */
class [[MONGO_MOD_PUBLIC]] DirectShardClientTracker {
    DirectShardClientTracker(const DirectShardClientTracker&) = delete;
    DirectShardClientTracker& operator=(const DirectShardClientTracker&) = delete;

public:
    static constexpr std::string_view kCurrentFieldName = "current"sv;
    static constexpr std::string_view kCreatedFieldName = "totalCreated"sv;

    DirectShardClientTracker() = default;
    ~DirectShardClientTracker() = default;

    static DirectShardClientTracker& get(ServiceContext* svcCtx);
    static DirectShardClientTracker& get(OperationContext* opCtx);

    class Token {
    public:
        explicit Token(ServiceContext* svcCtx);
        ~Token();

    private:
        ServiceContext* const _svcCtx = nullptr;
    };

    /*
     * Starts tracking this client. Internally asserts that this is a shard client.
     */
    static void trackClient(Client* client);

    void appendStats(BSONObjBuilder* bob) const;

private:
    Atomic<long long> _created{0};
    Atomic<long long> _destroyed{0};
};

}  // namespace mongo
