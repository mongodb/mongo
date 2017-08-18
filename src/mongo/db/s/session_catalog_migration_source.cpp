/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/s/session_catalog_migration_source.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_history_iterator.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

BSONObj fetchPrePostImageOplog(OperationContext* opCtx, const repl::OplogEntry& oplog) {
    auto tsToFetch = oplog.getPreImageTs();

    if (!tsToFetch) {
        tsToFetch = oplog.getPostImageTs();
    }

    if (!tsToFetch) {
        return BSONObj();
    }

    DBDirectClient client(opCtx);
    return client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                          Query(BSON("ts" << tsToFetch.value())));
}

}  // unnamed namespace

SessionCatalogMigrationSource::SessionCatalogMigrationSource(NamespaceString ns)
    : _ns(std::move(ns)) {}

bool SessionCatalogMigrationSource::hasMoreOplog() {
    return _hasMoreOplogFromSessionCatalog() || _hasNewWrites();
}

BSONObj SessionCatalogMigrationSource::getLastFetchedOplog() {
    {
        stdx::lock_guard<stdx::mutex> _lk(_sessionCloneMutex);
        if (!_lastFetchedOplog.isEmpty()) {
            return _lastFetchedOplog;
        }
    }

    {
        stdx::lock_guard<stdx::mutex> _lk(_newOplogMutex);
        invariant(!_lastFetchedNewWriteOplog.isEmpty());
        return _lastFetchedNewWriteOplog;
    }
}

bool SessionCatalogMigrationSource::fetchNextOplog(OperationContext* opCtx) {
    if (_fetchNextOplogFromSessionCatalog(opCtx)) {
        return true;
    }

    return _fetchNextNewWriteOplog(opCtx);
}

bool SessionCatalogMigrationSource::_handleWriteHistory(WithLock, OperationContext* opCtx) {
    if (_writeHistoryIterator) {
        if (_writeHistoryIterator->hasNext()) {
            auto nextOplog = _writeHistoryIterator->next(opCtx);

            // Note: This is an optimization based on the assumption that it is not possible to be
            // touching different namespaces in the same transaction.
            if (nextOplog.getNamespace() != _ns) {
                _writeHistoryIterator.reset();
                return false;
            }

            _lastFetchedOplog = nextOplog.toBSON().getOwned();

            auto doc = fetchPrePostImageOplog(opCtx, nextOplog);
            if (!doc.isEmpty()) {
                _lastFetchedOplogBuffer.push_back(doc);
            }

            return true;
        } else {
            _writeHistoryIterator.reset();
        }
    }

    return false;
}

bool SessionCatalogMigrationSource::_hasMoreOplogFromSessionCatalog() {
    stdx::lock_guard<stdx::mutex> _lk(_sessionCloneMutex);
    return !_lastFetchedOplog.isEmpty() || !_lastFetchedOplogBuffer.empty();
}

// Important: The no-op oplog entry for findAndModify should always be returned first before the
// actual operation.
BSONObj SessionCatalogMigrationSource::_getLastFetchedOplogFromSessionCatalog() {
    stdx::lock_guard<stdx::mutex> lk(_sessionCloneMutex);
    return _lastFetchedOplogBuffer.back();
}

bool SessionCatalogMigrationSource::_fetchNextOplogFromSessionCatalog(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_sessionCloneMutex);

    if (!_lastFetchedOplogBuffer.empty()) {
        _lastFetchedOplog = _lastFetchedOplogBuffer.back();
        _lastFetchedOplogBuffer.pop_back();
        return true;
    }

    _lastFetchedOplog = BSONObj();

    if (_handleWriteHistory(lk, opCtx)) {
        return true;
    }

    if (!_sessionCatalogCursor) {
        DBDirectClient client(opCtx);
        Query query;
        query.sort(BSON("_id" << 1));  // strictly not required, but helps make test deterministic.
        _sessionCatalogCursor =
            client.query(NamespaceString::kSessionTransactionsTableNamespace.ns(), query);
    }

    while (_sessionCatalogCursor->more()) {
        auto nextSession = SessionTxnRecord::parse(
            IDLParserErrorContext("Session migration cloning"), _sessionCatalogCursor->next());
        _writeHistoryIterator =
            stdx::make_unique<TransactionHistoryIterator>(nextSession.getLastWriteOpTimeTs());
        if (_handleWriteHistory(lk, opCtx)) {
            return true;
        }
    }

    return false;
}

bool SessionCatalogMigrationSource::_hasNewWrites() {
    stdx::lock_guard<stdx::mutex> lk(_newOplogMutex);
    return !_lastFetchedNewWriteOplog.isEmpty() || !_newWriteTsList.empty();
}

BSONObj SessionCatalogMigrationSource::_getLastFetchedNewWriteOplog() {
    stdx::lock_guard<stdx::mutex> lk(_newOplogMutex);
    return _lastFetchedNewWriteOplog;
}

bool SessionCatalogMigrationSource::_fetchNextNewWriteOplog(OperationContext* opCtx) {
    Timestamp nextOplogTsToFetch;

    {
        stdx::lock_guard<stdx::mutex> lk(_newOplogMutex);

        if (_newWriteTsList.empty()) {
            _lastFetchedNewWriteOplog = BSONObj();
            return false;
        }

        nextOplogTsToFetch = _newWriteTsList.front();
        _newWriteTsList.pop_front();
    }

    DBDirectClient client(opCtx);
    auto newWriteOplog = client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                                        Query(BSON("ts" << nextOplogTsToFetch)));

    uassert(40620,
            str::stream() << "Unable to fetch oplog entry with ts: " << nextOplogTsToFetch.toBSON(),
            !newWriteOplog.isEmpty());

    {
        stdx::lock_guard<stdx::mutex> lk(_newOplogMutex);
        _lastFetchedNewWriteOplog = newWriteOplog;
    }

    return true;
}

void SessionCatalogMigrationSource::notifyNewWriteTS(Timestamp opTimestamp) {
    stdx::lock_guard<stdx::mutex> lk(_newOplogMutex);
    _newWriteTsList.push_back(opTimestamp);
}

}  // namespace mongo
