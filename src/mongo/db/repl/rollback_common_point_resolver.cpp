/**
 *    Copyright (C) 2017 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/db/repl/rollback_common_point_resolver.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/abstract_oplog_fetcher.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Functions to extract OpTimes, timestamps, and hashes from oplog entries in various different
 * types.
 */
OpTime getOpTime(const OplogInterface::Iterator::Value& oplogValue) {
    return fassertStatusOK(40421, OpTime::parseFromOplogEntry(oplogValue.first));
}

Timestamp getTimestamp(const BSONObj& operation) {
    return operation["ts"].timestamp();
}

Timestamp getTimestamp(const OplogInterface::Iterator::Value& oplogValue) {
    return getTimestamp(oplogValue.first);
}

long long getHash(const BSONObj& operation) {
    return operation["h"].Long();
}

long long getHash(const OplogInterface::Iterator::Value& oplogValue) {
    return getHash(oplogValue.first);
}

/**
 * Returns a `getMore` command object suitable for traversing the remote oplog.
 */
BSONObj makeGetMoreCommandObject(const NamespaceString& nss,
                                 CursorId cursorId,
                                 Milliseconds fetcherMaxTimeMS) {
    BSONObjBuilder cmdBob;
    cmdBob.append("getMore", cursorId);
    cmdBob.append("collection", nss.coll());
    cmdBob.append("maxTimeMS", durationCount<Milliseconds>(fetcherMaxTimeMS));
    return cmdBob.obj();
}

}  // namespace

RollbackCommonPointResolver::RollbackCommonPointResolver(executor::TaskExecutor* executor,
                                                         HostAndPort source,
                                                         NamespaceString nss,
                                                         std::size_t maxFetcherRestarts,
                                                         OplogInterface* localOplog,
                                                         Listener* listener,
                                                         OnShutdownCallbackFn onShutdownCallbackFn)
    : AbstractOplogFetcher(
          executor,
          OpTimeWithHash(0, OpTime(Timestamp::max(), std::numeric_limits<long long>::max())),
          source,
          nss,
          maxFetcherRestarts,
          onShutdownCallbackFn,
          "rollback common point resolver"),
      _metadataObject(ReadPreferenceSetting::secondaryPreferredMetadata()),
      _localOplog(localOplog),
      _listener(listener) {

    invariant(_localOplog);
    invariant(listener);
}

RollbackCommonPointResolver::~RollbackCommonPointResolver() {
    shutdown();
    join();
}

Status RollbackCommonPointResolver::_doStartup_inlock() noexcept {
    Status abstractStartupStatus = AbstractOplogFetcher::_doStartup_inlock();
    if (abstractStartupStatus.isOK()) {
        _localOplogIterator = _localOplog->makeIterator();
    }
    return abstractStartupStatus;
}

BSONObj RollbackCommonPointResolver::_makeFindCommandObject(const NamespaceString& nss,
                                                            OpTime lastOpTimeFetched) const {
    BSONObjBuilder cmdBob;
    cmdBob.append("find", nss.coll());
    cmdBob.append("filter", BSON("ts" << BSON("$lt" << lastOpTimeFetched.getTimestamp())));
    cmdBob.append("sort", BSON("$natural" << -1));
    cmdBob.append("maxTimeMS",
                  durationCount<Milliseconds>(AbstractOplogFetcher::kOplogInitialFindMaxTime));
    return cmdBob.obj();
}

BSONObj RollbackCommonPointResolver::_makeMetadataObject() const {
    return _metadataObject;
}

StatusWith<OplogInterface::Iterator::Value> RollbackCommonPointResolver::_getNextLocalOplogEntry(
    const BSONObj& remoteOplogEntry) {
    auto localOplogResult = _localOplogIterator->next();
    if (!localOplogResult.isOK()) {
        return Status(ErrorCodes::NoMatchingDocument,
                      str::stream()
                          << "Rollback common point resolver reached beginning of local oplog "
                             "without finding common point. Locally scanned "
                          << _localScanned
                          << " to "
                          << getTimestamp(_localOplogValue).toString()
                          << ". Remotely scanned "
                          << _remoteScanned
                          << " to "
                          << getTimestamp(remoteOplogEntry).toString());
    }
    return localOplogResult.getValue();
}

StatusWith<BSONObj> RollbackCommonPointResolver::_onSuccessfulBatch(
    const Fetcher::QueryResponse& queryResponse) {
    const auto& documents = queryResponse.documents;

    // If we exit this callback and no longer need to read the local oplog, we destroy the iterator
    // to release its locks.
    auto resetLocalOplogIteratorGuard = MakeGuard([this] { _localOplogIterator.reset(); });

    // If we received an empty batch from the remote oplog, then there is no common point.
    //
    // TODO: If _localScanned == 0 and the local last fetched OpTime is less than the remote OpTime
    // including the term, then the entire remote oplog is ahead of the local one and
    // we can change sync sources instead of performing rollback.
    if (documents.empty()) {
        return Status(ErrorCodes::NoMatchingDocument,
                      "Rollback common point resolver reached beginning of remote oplog without "
                      "finding common point");
    }

    LOG(2) << "Rollback common point resolver read " << documents.size()
           << " operations from remote oplog starting at " << documents.front()["ts"]
           << " and ending at " << documents.back()["ts"];

    // If this is the first batch, then _localOplogValue will be empty, and we need to get
    // the first oplog entry off of the local oplog to begin. If there is no oplog then we fail
    // because we cannot find the common point.
    if (_remoteScanned == 0) {
        invariant(_localOplogValue.first.isEmpty());
        auto localOplogResult = _localOplogIterator->next();
        if (!localOplogResult.isOK()) {
            return Status(ErrorCodes::OplogStartMissing,
                          "No local oplog during rollback. Cannot find common point.");
        }
        _localOplogValue = localOplogResult.getValue();
    }

    for (auto remoteOplogEntry : documents) {
        // Process all local oplog entries that are greater than the current remote oplog entry.
        while (getTimestamp(_localOplogValue) > getTimestamp(remoteOplogEntry)) {
            _localScanned++;
            auto listenerStatus = _listener->onLocalOplogEntry(_localOplogValue.first);
            if (!listenerStatus.isOK()) {
                return listenerStatus;
            }

            // Get next local oplog entry and error if we've reached the end of the local oplog.
            auto localOplogResult = _getNextLocalOplogEntry(remoteOplogEntry);
            if (!localOplogResult.isOK()) {
                return localOplogResult.getStatus();
            }
            _localOplogValue = localOplogResult.getValue();
        }

        // If the local timestamp is equal to the remote timestamp, we may have the common point.
        if (getTimestamp(_localOplogValue) == getTimestamp(remoteOplogEntry)) {
            // The timestamps could be the same and still have different hashes because they are
            // from different branches of history.
            // In PV1 this would mean they have different terms.
            if (getHash(_localOplogValue) == getHash(remoteOplogEntry)) {
                // We've found the common point!
                auto listenerStatus = _listener->onCommonPoint(
                    std::make_pair(getOpTime(_localOplogValue), _localOplogValue.second));
                if (!listenerStatus.isOK()) {
                    return listenerStatus;
                }
                // Return an empty BSONObj as the `getMore` request as an indicator to stop
                // fetching.
                return BSONObj();
            }

            // Don't process the local oplog entry until after we know if it's the common point.
            _localScanned++;
            auto listenerStatus = _listener->onLocalOplogEntry(_localOplogValue.first);
            if (!listenerStatus.isOK()) {
                return listenerStatus;
            }

            // Get next local oplog entry and error if we've reached the end of the local oplog.
            auto localOplogResult = _getNextLocalOplogEntry(remoteOplogEntry);
            if (!localOplogResult.isOK()) {
                return localOplogResult.getStatus();
            }
            _localOplogValue = localOplogResult.getValue();
        }

        // Don't process the remote oplog entry until after we know if it's the common point.
        _remoteScanned++;
        auto listenerStatus = _listener->onRemoteOplogEntry(remoteOplogEntry);
        if (!listenerStatus.isOK()) {
            return listenerStatus;
        }

        invariant(getTimestamp(_localOplogValue) < getTimestamp(remoteOplogEntry));
    }

    // We need the local oplog iterator for the next batch, so don't destroy it.
    resetLocalOplogIteratorGuard.Dismiss();

    return makeGetMoreCommandObject(
        queryResponse.nss, queryResponse.cursorId, AbstractOplogFetcher::kOplogGetMoreMaxTime);
}

}  // namespace repl
}  // namespace mongo
