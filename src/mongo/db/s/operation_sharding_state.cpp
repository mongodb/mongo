/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/s/operation_sharding_state.h"

#include "mongo/db/operation_context.h"

namespace mongo {

namespace {

const OperationContext::Decoration<OperationShardingState> shardingMetadataDecoration =
    OperationContext::declareDecoration<OperationShardingState>();

// Max time to wait for the migration critical section to complete
const Milliseconds kMaxWaitForMigrationCriticalSection = Minutes(5);

// The name of the field in which the client attaches its database version.
constexpr auto kDbVersionField = "databaseVersion"_sd;
}  // namespace

OperationShardingState::OperationShardingState() = default;

OperationShardingState& OperationShardingState::get(OperationContext* opCtx) {
    return shardingMetadataDecoration(opCtx);
}

void OperationShardingState::setAllowImplicitCollectionCreation(
    const BSONElement& allowImplicitCollectionCreationElem) {
    if (!allowImplicitCollectionCreationElem.eoo()) {
        _allowImplicitCollectionCreation = allowImplicitCollectionCreationElem.Bool();
    } else {
        _allowImplicitCollectionCreation = true;
    }
}

bool OperationShardingState::allowImplicitCollectionCreation() const {
    return _allowImplicitCollectionCreation;
}

void OperationShardingState::initializeClientRoutingVersions(NamespaceString nss,
                                                             const BSONObj& cmdObj) {
    invariant(!hasShardVersion());
    invariant(!_dbVersion);

    _nss = std::move(nss);

    const auto shardVersionElem = cmdObj.getField(ChunkVersion::kShardVersionField);
    if (!shardVersionElem.eoo()) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "expected shardVersion element to be an array, got "
                              << shardVersionElem,
                shardVersionElem.type() == BSONType::Array);
        const BSONArray versionArr(shardVersionElem.Obj());

        bool canParse;
        ChunkVersion shardVersion = ChunkVersion::fromBSON(versionArr, &canParse);
        uassert(ErrorCodes::BadValue,
                str::stream() << "could not parse shardVersion from field " << versionArr,
                canParse);

        _hasShardVersion = true;
        if (nss.isSystemDotIndexes()) {
            _shardVersion = ChunkVersion::IGNORED();
        } else {
            _shardVersion = std::move(shardVersion);
        }
    }

    const auto dbVersionElem = cmdObj.getField(kDbVersionField);
    if (!dbVersionElem.eoo()) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "expected databaseVersion element to be an object, got "
                              << dbVersionElem,
                dbVersionElem.type() == BSONType::Object);
        _dbVersion = DatabaseVersion::parse(IDLParserErrorContext("setClientRoutingVersions"),
                                            dbVersionElem.Obj());
    }
}

bool OperationShardingState::hasShardVersion() const {
    return _hasShardVersion;
}

ChunkVersion OperationShardingState::getShardVersion(const NamespaceString& nss) const {
    if (_nss != nss) {
        return ChunkVersion::UNSHARDED();
    }

    return _shardVersion;
}

bool OperationShardingState::hasDbVersion() const {
    return !!_dbVersion;
}

boost::optional<DatabaseVersion> OperationShardingState::getDbVersion(const StringData db) const {
    if (_nss.db() != db) {
        return boost::none;
    }
    return _dbVersion;
}

void OperationShardingState::setShardVersion(NamespaceString nss, ChunkVersion newVersion) {
    // This currently supports only setting the shard version for one namespace.
    invariant(!_hasShardVersion || _nss == nss);
    invariant(!nss.isSystemDotIndexes() || ChunkVersion::isIgnoredVersion(newVersion));

    _nss = std::move(nss);
    _shardVersion = std::move(newVersion);
    _hasShardVersion = true;
}

bool OperationShardingState::waitForMigrationCriticalSectionSignal(OperationContext* opCtx) {
    // Must not block while holding a lock
    invariant(!opCtx->lockState()->isLocked());

    if (_migrationCriticalSectionSignal) {
        _migrationCriticalSectionSignal->waitFor(
            opCtx,
            opCtx->hasDeadline()
                ? std::min(opCtx->getRemainingMaxTimeMillis(), kMaxWaitForMigrationCriticalSection)
                : kMaxWaitForMigrationCriticalSection);
        _migrationCriticalSectionSignal = nullptr;
        return true;
    }

    return false;
}

void OperationShardingState::setMigrationCriticalSectionSignal(
    std::shared_ptr<Notification<void>> critSecSignal) {
    invariant(critSecSignal);
    _migrationCriticalSectionSignal = std::move(critSecSignal);
}

}  // namespace mongo
