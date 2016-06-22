/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/ops/parsed_delete.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

ParsedDelete::ParsedDelete(OperationContext* txn, const DeleteRequest* request)
    : _txn(txn), _request(request) {}

Status ParsedDelete::parseRequest() {
    dassert(!_canonicalQuery.get());
    // It is invalid to request that the DeleteStage return the deleted document during a
    // multi-remove.
    invariant(!(_request->shouldReturnDeleted() && _request->isMulti()));

    // It is invalid to request that a ProjectionStage be applied to the DeleteStage if the
    // DeleteStage would not return the deleted document.
    invariant(_request->getProj().isEmpty() || _request->shouldReturnDeleted());

    if (CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
        return Status::OK();
    }

    return parseQueryToCQ();
}

Status ParsedDelete::parseQueryToCQ() {
    dassert(!_canonicalQuery.get());

    const ExtensionsCallbackReal extensionsCallback(_txn, &_request->getNamespaceString());

    // The projection needs to be applied after the delete operation, so we do not specify a
    // projection during canonicalization.
    auto qr = stdx::make_unique<QueryRequest>(_request->getNamespaceString());
    qr->setFilter(_request->getQuery());
    qr->setSort(_request->getSort());
    qr->setCollation(_request->getCollation());
    qr->setExplain(_request->isExplain());

    // Limit should only used for the findAndModify command when a sort is specified. If a sort
    // is requested, we want to use a top-k sort for efficiency reasons, so should pass the
    // limit through. Generally, a delete stage expects to be able to skip documents that were
    // deleted out from under it, but a limit could inhibit that and give an EOF when the delete
    // has not actually deleted a document. This behavior is fine for findAndModify, but should
    // not apply to deletes in general.
    if (!_request->isMulti() && !_request->getSort().isEmpty()) {
        qr->setLimit(1);
    }

    auto statusWithCQ = CanonicalQuery::canonicalize(_txn, std::move(qr), extensionsCallback);

    if (statusWithCQ.isOK()) {
        _canonicalQuery = std::move(statusWithCQ.getValue());
    }

    return statusWithCQ.getStatus();
}

const DeleteRequest* ParsedDelete::getRequest() const {
    return _request;
}

PlanExecutor::YieldPolicy ParsedDelete::yieldPolicy() const {
    if (_request->isGod()) {
        return PlanExecutor::YIELD_MANUAL;
    }
    if (_request->getYieldPolicy() == PlanExecutor::YIELD_AUTO && isIsolated()) {
        return PlanExecutor::WRITE_CONFLICT_RETRY_ONLY;  // Don't yield locks.
    }
    return _request->getYieldPolicy();
}

bool ParsedDelete::isIsolated() const {
    return _canonicalQuery.get() ? _canonicalQuery->isIsolated()
                                 : QueryRequest::isQueryIsolated(_request->getQuery());
}

bool ParsedDelete::hasParsedQuery() const {
    return _canonicalQuery.get() != NULL;
}

std::unique_ptr<CanonicalQuery> ParsedDelete::releaseParsedQuery() {
    invariant(_canonicalQuery.get() != NULL);
    return std::move(_canonicalQuery);
}

}  // namespace mongo
