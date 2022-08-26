/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class StaleConfigInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleConfig;
    enum class OperationType { kRead, kWrite };

    StaleConfigInfo(NamespaceString nss,
                    ShardVersion received,
                    boost::optional<ShardVersion> wanted,
                    ShardId shardId,
                    boost::optional<SharedSemiFuture<void>> criticalSectionSignal = boost::none,
                    boost::optional<OperationType> duringOperationType = boost::none)
        : _nss(std::move(nss)),
          _received(received),
          _wanted(wanted),
          _shardId(shardId),
          _criticalSectionSignal(std::move(criticalSectionSignal)),
          _duringOperationType{duringOperationType} {}

    const auto& getNss() const {
        return _nss;
    }

    const auto& getVersionReceived() const {
        return _received;
    }

    const auto& getVersionWanted() const {
        return _wanted;
    }

    const auto& getShardId() const {
        return _shardId;
    }

    auto getCriticalSectionSignal() const {
        return _criticalSectionSignal;
    }

    const auto& getDuringOperationType() const {
        return _duringOperationType;
    }

    void serialize(BSONObjBuilder* bob) const;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

protected:
    NamespaceString _nss;
    ShardVersion _received;
    boost::optional<ShardVersion> _wanted;
    ShardId _shardId;

    // The following fields are not serialized and therefore do not get propagated to the router.
    boost::optional<SharedSemiFuture<void>> _criticalSectionSignal;
    boost::optional<OperationType> _duringOperationType;
};

class StaleEpochInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleEpoch;

    StaleEpochInfo(NamespaceString nss) : _nss(std::move(nss)) {}

    const auto& getNss() const {
        return _nss;
    }

    void serialize(BSONObjBuilder* bob) const;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    NamespaceString _nss;
};

using StaleConfigException = ExceptionFor<ErrorCodes::StaleConfig>;

class StaleDbRoutingVersion final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleDbVersion;

    StaleDbRoutingVersion(
        std::string db,
        DatabaseVersion received,
        boost::optional<DatabaseVersion> wanted,
        boost::optional<SharedSemiFuture<void>> criticalSectionSignal = boost::none)
        : _db(std::move(db)),
          _received(received),
          _wanted(wanted),
          _criticalSectionSignal(std::move(criticalSectionSignal)) {}

    const auto& getDb() const {
        return _db;
    }

    const auto& getVersionReceived() const {
        return _received;
    }

    const auto& getVersionWanted() const {
        return _wanted;
    }

    auto getCriticalSectionSignal() const {
        return _criticalSectionSignal;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    std::string _db;
    DatabaseVersion _received;
    boost::optional<DatabaseVersion> _wanted;

    // This signal does not get serialized and therefore does not get propagated to the router
    boost::optional<SharedSemiFuture<void>> _criticalSectionSignal;
};

}  // namespace mongo
