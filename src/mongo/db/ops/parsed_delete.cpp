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
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    ParsedDelete::ParsedDelete(OperationContext* txn, const DeleteRequest* request) :
        _txn(txn),
        _request(request) { }

    Status ParsedDelete::parseRequest() {
        dassert(!_canonicalQuery.get());

        if (CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
            return Status::OK();
        }

        return parseQueryToCQ();
    }

    Status ParsedDelete::parseQueryToCQ() {
        dassert(!_canonicalQuery.get());

        CanonicalQuery* cqRaw;
        const WhereCallbackReal whereCallback(_txn, _request->getNamespaceString().db());

        Status status = CanonicalQuery::canonicalize(_request->getNamespaceString().ns(),
                                                     _request->getQuery(),
                                                     _request->isExplain(),
                                                     &cqRaw,
                                                     whereCallback);

        if (status.isOK()) {
            cqRaw->setIsForWrite(true);
            _canonicalQuery.reset(cqRaw);
        }

        return status;
    }

    const DeleteRequest* ParsedDelete::getRequest() const {
        return _request;
    }

    bool ParsedDelete::canYield() const {
        return !_request->isGod() &&
            PlanExecutor::YIELD_AUTO == _request->getYieldPolicy() && (
            _canonicalQuery.get() ?
            !QueryPlannerCommon::hasNode(_canonicalQuery->root(), MatchExpression::ATOMIC) :
            !LiteParsedQuery::isQueryIsolated(_request->getQuery()));
    }

    bool ParsedDelete::hasParsedQuery() const {
        return _canonicalQuery.get() != NULL;
    }

    CanonicalQuery* ParsedDelete::releaseParsedQuery() {
        invariant(_canonicalQuery.get() != NULL);
        return _canonicalQuery.release();
    }

}  // namespace mongo
