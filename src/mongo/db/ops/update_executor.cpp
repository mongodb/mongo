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

#include "mongo/platform/basic.h"

#include "mongo/db/ops/update_executor.h"

#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    UpdateExecutor::UpdateExecutor(const UpdateRequest* request, OpDebug* opDebug) :
        _request(request),
        _opDebug(opDebug),
        _driver(UpdateDriver::Options()),
        _canonicalQuery(),
        _isQueryParsed(false),
        _isUpdateParsed(false) {
    }

    UpdateExecutor::~UpdateExecutor() {}

    Status UpdateExecutor::prepare() {
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

    UpdateResult UpdateExecutor::execute(TransactionExperiment* txn, Database* db) {
        uassertStatusOK(prepare());
        return update(txn,
                      db,
                      *_request,
                      _opDebug,
                      &_driver,
                      _canonicalQuery.release());
    }

    Status UpdateExecutor::parseQuery() {
        if (_isQueryParsed)
            return Status::OK();

        dassert(!_canonicalQuery.get());
        dassert(_isUpdateParsed);

        if (!_driver.needMatchDetails() && CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
            _isQueryParsed = true;
            return Status::OK();
        }

        CanonicalQuery* cqRaw;
        Status status = CanonicalQuery::canonicalize(_request->getNamespaceString().ns(),
                                                     _request->getQuery(),
                                                     &cqRaw);
        if (status.isOK()) {
            _canonicalQuery.reset(cqRaw);
            _isQueryParsed = true;
        }
        else if (status == ErrorCodes::NoClientContext) {
            // _isQueryParsed is still false, but execute() will try again under the lock.
            return status = Status::OK();
        }
        return status;
    }

    Status UpdateExecutor::parseUpdate() {
        if (_isUpdateParsed)
            return Status::OK();

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
        Status status = _driver.parse(_request->getUpdates(), _request->isMulti());
        if (status.isOK())
            _isUpdateParsed = true;
        return status;
    }

}  // namespace mongo
