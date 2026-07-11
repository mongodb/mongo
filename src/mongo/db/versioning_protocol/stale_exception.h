// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/critical_section_signal.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] StaleConfigInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleConfig;
    enum class OperationType { kRead, kWrite };

    StaleConfigInfo(NamespaceString nss,
                    ShardVersion received,
                    boost::optional<ShardVersion> wanted,
                    ShardRef shardRef,
                    boost::optional<CriticalSectionSignal> criticalSectionSignal = boost::none,
                    boost::optional<OperationType> duringOperationType = boost::none)
        : _nss(std::move(nss)),
          _received(received),
          _wanted(wanted),
          _shardRef(std::move(shardRef)),
          _criticalSectionSignal(std::move(criticalSectionSignal)),
          _duringOperationType{duringOperationType} {
        tassert(11993000, "Invalid ShardRef.", ShardRef::validate(_shardRef).isOK());
    }

    const auto& getNss() const {
        return _nss;
    }

    const auto& getVersionReceived() const {
        return _received;
    }

    const auto& getVersionWanted() const {
        return _wanted;
    }

    const auto& getShardRef() const {
        return _shardRef;
    }

    auto getCriticalSectionSignal() const {
        return _criticalSectionSignal;
    }

    const auto& getDuringOperationType() const {
        return _duringOperationType;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    NamespaceString _nss;
    ShardVersion _received;
    boost::optional<ShardVersion> _wanted;
    ShardRef _shardRef;

    // The following fields are not serialized and therefore do not get propagated to the router.
    boost::optional<CriticalSectionSignal> _criticalSectionSignal;
    boost::optional<OperationType> _duringOperationType;
};

// TODO (SERVER-75888): Rename the StaleEpoch code to StaleUpstreamRouter and the info to
// StaleUpstreamRouterInfo
class [[MONGO_MOD_NEEDS_REPLACEMENT]] StaleEpochInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleEpoch;

    StaleEpochInfo(NamespaceString nss,
                   boost::optional<ShardVersion> received = boost::none,
                   boost::optional<ShardVersion> wanted = boost::none)
        : _nss(std::move(nss)), _received(received), _wanted(wanted) {}

    const auto& getNss() const {
        return _nss;
    }

    const auto& getVersionReceived() const {
        return _received;
    }

    const auto& getVersionWanted() const {
        return _wanted;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

private:
    NamespaceString _nss;

    boost::optional<ShardVersion> _received;
    boost::optional<ShardVersion> _wanted;
};

class [[MONGO_MOD_NEEDS_REPLACEMENT]] StaleDbRoutingVersion final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::StaleDbVersion;

    StaleDbRoutingVersion(
        const DatabaseName& db,
        DatabaseVersion received,
        boost::optional<DatabaseVersion> wanted,
        boost::optional<CriticalSectionSignal> criticalSectionSignal = boost::none)
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
    DatabaseName _db;
    DatabaseVersion _received;
    boost::optional<DatabaseVersion> _wanted;

    // This signal does not get serialized and therefore does not get propagated to the router
    boost::optional<CriticalSectionSignal> _criticalSectionSignal;
};

/*
 * Returns true if 'errorCode' corresponds to an error related to stale sharding metadata.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] bool isStaleShardingMetadataError(ErrorCodes::Error errorCode);

}  // namespace mongo
