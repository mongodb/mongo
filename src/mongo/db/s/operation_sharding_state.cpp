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

#include "mongo/db/s/operation_sharding_state.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/s/sharding_api_d_params_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/database_name_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

const OperationContext::Decoration<OperationShardingState> shardingMetadataDecoration =
    OperationContext::declareDecoration<OperationShardingState>();

}  // namespace

OperationShardingState::OperationShardingState() = default;

OperationShardingState::~OperationShardingState() {
    invariant(!_shardingOperationFailedStatus);
}

OperationShardingState& OperationShardingState::get(OperationContext* opCtx) {
    return shardingMetadataDecoration(opCtx);
}

bool OperationShardingState::isComingFromRouter(OperationContext* opCtx) {
    const auto& oss = get(opCtx);
    return !oss._databaseVersions.empty() || !oss._shardVersions.empty();
}

bool OperationShardingState::shouldBeTreatedAsFromRouter(OperationContext* opCtx) {
    const auto& oss = get(opCtx);
    return !oss._databaseVersions.empty() || !oss._shardVersions.empty() || oss._treatAsFromRouter;
}

void OperationShardingState::setShardRole(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const boost::optional<ShardVersion>& shardVersion,
                                          const boost::optional<DatabaseVersion>& databaseVersion) {
    auto& oss = OperationShardingState::get(opCtx);

    if (shardVersion && shardVersion != ShardVersion::UNSHARDED()) {
        // TODO (SERVER-87196): remove the fcvSnapshot branch after 8.0 is released
        const auto fcvSnapshot = serverGlobalParams.featureCompatibility.acquireFCVSnapshot();
        // TODO (SERVER-87869): Change the FCV constant comparison to feature flag gating.
        if (fcvSnapshot.isVersionInitialized() &&
            fcvSnapshot.isGreaterThan(                       // NOLINT(mongo-fcv-constant-check)
                multiversion::FeatureCompatibilityVersion::  // NOLINT(mongo-fcv-constant-check)
                kVersion_7_3)) {                             // NOLINT(mongo-fcv-constant-check)
            tassert(6300900,
                    "Attaching a shard version requires a non db-only namespace",
                    !nss.isDbOnly());
        }
    }

    bool shardVersionInserted = false;
    bool databaseVersionInserted = false;
    try {
        boost::optional<OperationShardingState::ShardVersionTracker&> shardVersionTracker;
        if (shardVersion) {
            auto emplaceResult = oss._shardVersions.try_emplace(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()),
                *shardVersion);
            shardVersionInserted = emplaceResult.second;
            shardVersionTracker = emplaceResult.first->second;
            if (!shardVersionInserted) {
                uassert(ErrorCodes::IllegalChangeToExpectedShardVersion,
                        str::stream() << "Illegal attempt to change the expected shard version for "
                                      << nss.toStringForErrorMsg() << " from "
                                      << shardVersionTracker->v << " to " << *shardVersion
                                      << " at recursion level " << shardVersionTracker->recursion,
                        shardVersionTracker->v == *shardVersion);
                invariant(shardVersionTracker->recursion > 0);
            } else {
                invariant(shardVersionTracker->recursion == 0);
            }
        }

        boost::optional<OperationShardingState::DatabaseVersionTracker&> dbVersionTracker;
        if (databaseVersion) {
            auto emplaceResult = oss._databaseVersions.try_emplace(nss.dbName(), *databaseVersion);
            databaseVersionInserted = emplaceResult.second;
            dbVersionTracker = emplaceResult.first->second;
            if (!databaseVersionInserted) {
                uassert(ErrorCodes::IllegalChangeToExpectedDatabaseVersion,
                        str::stream()
                            << "Illegal attempt to change the expected database version for "
                            << nss.dbName().toStringForErrorMsg() << " from " << dbVersionTracker->v
                            << " to " << *databaseVersion << " at recursion level "
                            << dbVersionTracker->recursion,
                        dbVersionTracker->v == *databaseVersion);
                invariant(dbVersionTracker->recursion > 0);
            } else {
                invariant(dbVersionTracker->recursion == 0);
            }
        }

        // Update the recursion at the end to preserve the strong exception guarantee.
        if (shardVersionTracker) {
            shardVersionTracker->recursion++;
        }
        if (dbVersionTracker) {
            dbVersionTracker->recursion++;
        }

    } catch (const DBException&) {
        // Clean any oss update done within this method on failure to get a strong exception
        // guarantee on ScopedSetShardRole objects.
        if (shardVersionInserted) {
            oss._shardVersions.erase(
                NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
        }
        if (databaseVersionInserted) {
            oss._databaseVersions.erase(nss.dbName());
        }

        throw;
    }
}

boost::optional<ShardVersion> OperationShardingState::getShardVersion(const NamespaceString& nss) {
    const auto it = _shardVersions.find(
        NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    if (it != _shardVersions.end()) {
        return it->second.v;
    }
    return boost::none;
}

boost::optional<DatabaseVersion> OperationShardingState::getDbVersion(
    const DatabaseName& dbName) const {
    const auto it = _databaseVersions.find(dbName);
    if (it != _databaseVersions.end()) {
        return it->second.v;
    }
    return boost::none;
}

Status OperationShardingState::waitForCriticalSectionToComplete(
    OperationContext* opCtx, SharedSemiFuture<void> critSecSignal) noexcept {
    // Must not block while holding a lock
    invariant(!shard_role_details::getLocker(opCtx)->isLocked());

    // If we are in a transaction, limit the time we can wait behind the critical section. This is
    // needed in order to prevent distributed deadlocks in situations where a DDL operation needs to
    // acquire the critical section on several shards.
    //
    // In such cases, shard running a transaction could be waiting for the critical section to be
    // exited, while on another shard the transaction has already executed some statement and
    // stashed locks which prevent the critical section from being acquired in that node. Limiting
    // the wait behind the critical section will ensure that the transaction will eventually get
    // aborted.
    if (opCtx->inMultiDocumentTransaction()) {
        try {
            opCtx->runWithDeadline(
                opCtx->getServiceContext()->getFastClockSource()->now() +
                    Milliseconds(metadataRefreshInTransactionMaxWaitBehindCritSecMS.load()),
                ErrorCodes::ExceededTimeLimit,
                [&] { critSecSignal.wait(opCtx); });
            return Status::OK();
        } catch (const DBException& ex) {
            // This is a best-effort attempt to wait for the critical section to complete, so no
            // need to handle any exceptions
            return ex.toStatus();
        }
    } else {
        return critSecSignal.waitNoThrow(opCtx);
    }
}

void OperationShardingState::setShardingOperationFailedStatus(const Status& status) {
    invariant(!_shardingOperationFailedStatus);
    _shardingOperationFailedStatus = status;
}

boost::optional<Status> OperationShardingState::resetShardingOperationFailedStatus() {
    if (!_shardingOperationFailedStatus) {
        return boost::none;
    }
    Status failedStatus = Status(*_shardingOperationFailedStatus);
    _shardingOperationFailedStatus = boost::none;
    return failedStatus;
}

using ScopedAllowImplicitCollectionCreate_UNSAFE =
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE;

ScopedAllowImplicitCollectionCreate_UNSAFE::ScopedAllowImplicitCollectionCreate_UNSAFE(
    OperationContext* opCtx, bool forceCSRAsUnknownAfterCollectionCreation)
    : _opCtx(opCtx) {
    auto& oss = get(_opCtx);
    // TODO (SERVER-82066): Re-enable invariant if possible after updating direct connection
    // handling.
    // invariant(!oss._allowCollectionCreation);
    oss._allowCollectionCreation = true;
    oss._forceCSRAsUnknownAfterCollectionCreation = forceCSRAsUnknownAfterCollectionCreation;
}

ScopedAllowImplicitCollectionCreate_UNSAFE::~ScopedAllowImplicitCollectionCreate_UNSAFE() {
    auto& oss = get(_opCtx);
    // TODO (SERVER-82066): Re-enable invariant if possible after updating direct connection
    // handling.
    // invariant(oss._allowCollectionCreation);
    oss._allowCollectionCreation = false;
    oss._forceCSRAsUnknownAfterCollectionCreation = false;
}

ScopedSetShardRole::ScopedSetShardRole(OperationContext* opCtx,
                                       NamespaceString nss,
                                       boost::optional<ShardVersion> shardVersion,
                                       boost::optional<DatabaseVersion> databaseVersion)
    : _opCtx(opCtx),
      _nss(std::move(nss)),
      _shardVersion(std::move(shardVersion)),
      _databaseVersion(std::move(databaseVersion)) {
    // "Fixed" dbVersions are only used for dbs that don't have the sharding infrastructure set up
    // to handle real database or shard versions (like config and admin), so we ignore them.
    if (_databaseVersion && _databaseVersion->isFixed()) {
        uassert(7331300,
                "A 'fixed' dbVersion should only be used with an unsharded shard version or none "
                "at all",
                !_shardVersion || _shardVersion == ShardVersion::UNSHARDED());
        _databaseVersion.reset();
        _shardVersion.reset();
    }

    OperationShardingState::setShardRole(_opCtx, _nss, _shardVersion, _databaseVersion);
}

ScopedSetShardRole::ScopedSetShardRole(ScopedSetShardRole&& other)
    : _opCtx(other._opCtx),
      _nss(std::move(other._nss)),
      _shardVersion(std::move(other._shardVersion)),
      _databaseVersion(std::move(other._databaseVersion)) {
    // Clear the _shardVersion/_databaseVersion of 'other'; this prevents modifying
    // OperationShardingState on destruction of the moved from object.
    other._shardVersion.reset();
    other._databaseVersion.reset();
}

ScopedSetShardRole::~ScopedSetShardRole() {
    auto& oss = OperationShardingState::get(_opCtx);

    if (_shardVersion) {
        auto it = oss._shardVersions.find(
            NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));
        invariant(it != oss._shardVersions.end());
        auto& tracker = it->second;
        invariant(--tracker.recursion >= 0);
        if (tracker.recursion == 0)
            oss._shardVersions.erase(it);
    }

    if (_databaseVersion) {
        auto it = oss._databaseVersions.find(_nss.dbName());
        invariant(it != oss._databaseVersions.end());
        auto& tracker = it->second;
        invariant(--tracker.recursion >= 0);
        if (tracker.recursion == 0)
            oss._databaseVersions.erase(it);
    }
}

ScopedStashShardRole::ScopedStashShardRole(OperationContext* opCtx, const NamespaceString& nss)
    : _opCtx(opCtx), _nss(nss) {
    auto& oss = OperationShardingState::get(_opCtx);

    const auto shardVersionIt = oss._shardVersions.find(
        NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));

    const auto dbVersionIt = oss._databaseVersions.find(_nss.dbName());

    // Check recursion preconditions first. Do the checks before modifying any
    // OperationShardingState to ensure upholding the strong exception guarantee.
    if (shardVersionIt != oss._shardVersions.end()) {
        tassert(8541900,
                "Cannot unset implicit views shard role if recursion level is greater than 1",
                shardVersionIt->second.recursion == 1);
    }

    if (dbVersionIt != oss._databaseVersions.end()) {
        tassert(8541901,
                "Cannot unset implicit views shard role if recursion level is greater than 1",
                dbVersionIt->second.recursion == 1);
    }

    // Stash shard/db versions.
    if (shardVersionIt != oss._shardVersions.end()) {
        _stashedShardVersion.emplace(shardVersionIt->second.v);
        oss._shardVersions.erase(shardVersionIt);
    }

    if (dbVersionIt != oss._databaseVersions.end()) {
        _stashedDatabaseVersion.emplace(dbVersionIt->second.v);
        oss._databaseVersions.erase(dbVersionIt);
    }
}

ScopedStashShardRole::~ScopedStashShardRole() {
    auto& oss = OperationShardingState::get(_opCtx);
    const auto shardVersionIt = oss._shardVersions.find(
        NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()));
    invariant(shardVersionIt == oss._shardVersions.end());

    if (_stashedShardVersion) {
        auto emplaceResult = oss._shardVersions.emplace(
            NamespaceStringUtil::serialize(_nss, SerializationContext::stateDefault()),
            *_stashedShardVersion);
        auto& tracker = emplaceResult.first->second;
        tracker.recursion = 1;
    }

    const auto dbVersionIt = oss._databaseVersions.find(_nss.dbName());
    invariant(dbVersionIt == oss._databaseVersions.end());
    if (_stashedDatabaseVersion) {
        auto emplaceResult = oss._databaseVersions.emplace(_nss.dbName(), *_stashedDatabaseVersion);
        auto& tracker = emplaceResult.first->second;
        tracker.recursion = 1;
    }
}

}  // namespace mongo
