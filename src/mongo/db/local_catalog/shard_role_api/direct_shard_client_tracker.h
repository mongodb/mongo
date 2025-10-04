/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

/**
 * Tracks the number of external clients on the shard port of a mongod.
 */
class DirectShardClientTracker {
    DirectShardClientTracker(const DirectShardClientTracker&) = delete;
    DirectShardClientTracker& operator=(const DirectShardClientTracker&) = delete;

public:
    static constexpr StringData kCurrentFieldName = "current"_sd;
    static constexpr StringData kCreatedFieldName = "totalCreated"_sd;

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
    AtomicWord<long long> _created{0};
    AtomicWord<long long> _destroyed{0};
};

}  // namespace mongo
