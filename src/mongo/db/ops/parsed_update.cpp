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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrites

#include "mongo/platform/basic.h"

#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/query_planner_common.h"

namespace mongo {

    ParsedUpdate::ParsedUpdate(OperationContext* txn, const UpdateRequest* request) :
        _txn(txn),
        _request(request),
        _driver(UpdateDriver::Options()),
        _canonicalQuery() { }

    Status ParsedUpdate::parseRequest() {
        // We parse the update portion before the query portion because the dispostion of the update
        // may determine whether or not we need to produce a CanonicalQuery at all.  For example, if
        // the update involves the positional-dollar operator, we must have a CanonicalQuery even if
        // it isn't required for query execution.
        Status status = parseUpdate();
        if (!status.isOK())
            return status;
        status = parseQuery();
        if (!status.isOK())
            return status;
        return Status::OK();
    }

    Status ParsedUpdate::parseQuery() {
        dassert(!_canonicalQuery.get());

        if (!_driver.needMatchDetails() && CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
            return Status::OK();
        }

        return parseQueryToCQ();
    }

    Status ParsedUpdate::parseQueryToCQ() {
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

    Status ParsedUpdate::parseUpdate() {
        const NamespaceString& ns(_request->getNamespaceString());

        // Should the modifiers validate their embedded docs via okForStorage
        // Only user updates should be checked. Any system or replication stuff should pass through.
        // Config db docs shouldn't get checked for valid field names since the shard key can have
        // a dot (".") in it.
        const bool shouldValidate = !(_request->isFromReplication() ||
                                      ns.isConfigDB() ||
                                      _request->isFromMigration());

        _driver.setLogOp(true);
        _driver.setModOptions(ModifierInterface::Options(_request->isFromReplication(),
                                                         shouldValidate));

        return _driver.parse(_request->getUpdates(), _request->isMulti());
    }

    bool ParsedUpdate::canYield() const {
        return !_request->isGod() &&
            PlanExecutor::YIELD_AUTO == _request->getYieldPolicy() && (
            _canonicalQuery.get() ?
            !QueryPlannerCommon::hasNode(_canonicalQuery->root(), MatchExpression::ATOMIC) :
            !LiteParsedQuery::isQueryIsolated(_request->getQuery()));
    }

    bool ParsedUpdate::hasParsedQuery() const {
        return _canonicalQuery.get() != NULL;
    }

    CanonicalQuery* ParsedUpdate::releaseParsedQuery() {
        invariant(_canonicalQuery.get() != NULL);
        return _canonicalQuery.release();
    }

    const UpdateRequest* ParsedUpdate::getRequest() const {
        return _request;
    }

    UpdateDriver* ParsedUpdate::getDriver() {
        return &_driver;
    }

}  // namespace mongo
