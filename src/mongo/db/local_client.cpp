/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/local_client.h"

#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/get_executor.h"

namespace mongo {

LocalClient::LocalClient(OperationContext* txn) : _txn(txn) {}

StatusWith<LocalClient::LocalCursor> LocalClient::query(const NamespaceString& nss,
                                                        const BSONObj& query,
                                                        const BSONObj& sort) {
    LocalCursor localCursor(_txn, nss, query, sort);
    Status initStatus = localCursor._init();
    if (!initStatus.isOK()) {
        return initStatus;
    }
    return std::move(localCursor);
}

LocalClient::LocalCursor::LocalCursor(OperationContext* txn,
                                      const NamespaceString& nss,
                                      const BSONObj& query,
                                      const BSONObj& sort)
    : _txn(txn), _nss(nss), _query(query), _sort(sort) {}

#if defined(_MSC_VER) && _MSC_VER < 1900
LocalClient::LocalCursor::LocalCursor(LocalCursor&& other)
    : _txn(std::move(other._txn)),
      _nss(std::move(other._nss)),
      _query(std::move(other._query)),
      _sort(std::move(other._sort)),
      _exec(std::move(other._exec)) {}

LocalClient::LocalCursor& LocalClient::LocalCursor::operator=(LocalCursor&& other) {
    _txn = std::move(other._txn);
    _nss = std::move(other._nss);
    _query = std::move(other._query);
    _sort = std::move(other._sort);
    _exec = std::move(other._exec);
    return *this;
}
#endif

Status LocalClient::LocalCursor::_init() {
    // This function does its own locking, so the lock should not be held.
    invariant(!_txn->lockState()->isLocked());

    if (!_nss.isValid()) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid collection name: '" << _nss.ns()};
    }

    auto lpq = LiteParsedQuery::makeAsFindCmd(_nss,
                                              _query,
                                              BSONObj(),  // projection
                                              _sort);
    ExtensionsCallbackReal extensionsCallback(_txn, &_nss);

    auto canonicalQuery = CanonicalQuery::canonicalize(lpq.release(), extensionsCallback);
    if (!canonicalQuery.isOK()) {
        return canonicalQuery.getStatus();
    }

    AutoGetCollection ctx(_txn, _nss, MODE_IS);
    Collection* collection = ctx.getCollection();

    auto statusWithPlanExecutor = getExecutor(
        _txn, collection, std::move(canonicalQuery.getValue()), PlanExecutor::YIELD_AUTO);
    if (!statusWithPlanExecutor.isOK()) {
        return statusWithPlanExecutor.getStatus();
    }
    _exec = std::move(statusWithPlanExecutor.getValue());
    _exec->saveState();

    return Status::OK();
}

StatusWith<boost::optional<BSONObj>> LocalClient::LocalCursor::next() {
    // If _exec is not set, an error occurred or the cursor was exhausted on the previous call.
    invariant(_exec);
    // This function does its own locking, so the lock should not be held.
    invariant(!_txn->lockState()->isLocked());
    AutoGetCollection ctx(_txn, _nss, MODE_IS);

    if (!_exec->restoreState()) {
        _exec.reset();
        return {ErrorCodes::OperationFailed,
                str::stream()
                    << "PlanExecutor could not be restored in LocalClient::LocalCursor::next()"};
    }

    BSONObj doc;
    PlanExecutor::ExecState state = _exec->getNext(&doc, NULL);

    if (state == PlanExecutor::IS_EOF) {
        _exec.reset();
        return {boost::none};
    }

    if (state != PlanExecutor::ADVANCED) {
        _exec.reset();
        return {ErrorCodes::OperationFailed,
                str::stream() << "PlanExecutor error: " << WorkingSetCommon::toStatusString(doc)};
    }

    doc = doc.getOwned();
    _exec->saveState();

    return {doc};
}

}  // namespace mongo
