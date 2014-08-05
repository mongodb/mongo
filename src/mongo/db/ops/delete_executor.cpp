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

#include "mongo/db/ops/delete_executor.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    DeleteExecutor::DeleteExecutor(const DeleteRequest* request) :
        _request(request),
        _canonicalQuery(),
        _isQueryParsed(false) {
    }

    DeleteExecutor::~DeleteExecutor() {}

    Status DeleteExecutor::prepare() {
        if (_isQueryParsed)
            return Status::OK();

        dassert(!_canonicalQuery.get());

        if (CanonicalQuery::isSimpleIdQuery(_request->getQuery())) {
            _isQueryParsed = true;
            return Status::OK();
        }

        CanonicalQuery* cqRaw;
        const WhereCallbackReal whereCallback(
                                    _request->getOpCtx(), _request->getNamespaceString().db());

        Status status = CanonicalQuery::canonicalize(_request->getNamespaceString().ns(),
                                                     _request->getQuery(),
                                                     &cqRaw,
                                                     whereCallback);
        if (status.isOK()) {
            _canonicalQuery.reset(cqRaw);
            _isQueryParsed = true;
        }

        return status;
    }

    long long DeleteExecutor::execute(Database* db) {
        uassertStatusOK(prepare());
        uassert(17417,
                mongoutils::str::stream() <<
                "DeleteExecutor::prepare() failed to parse query " << _request->getQuery(),
                _isQueryParsed);

        const NamespaceString& ns(_request->getNamespaceString());
        if (!_request->isGod()) {
            if (ns.isSystem()) {
                uassert(12050,
                        "cannot delete from system namespace",
                        legalClientSystemNS(ns.ns(), true));
            }
            if (ns.ns().find('$') != string::npos) {
                log() << "cannot delete from collection with reserved $ in name: " << ns << endl;
                uasserted(10100, "cannot delete from collection with reserved $ in name");
            }
        }

        Collection* collection = db->getCollection(_request->getOpCtx(), ns.ns());
        if (NULL == collection) {
            return 0;
        }

        uassert(10101,
                str::stream() << "cannot remove from a capped collection: " << ns.ns(),
                !collection->isCapped());

        uassert(ErrorCodes::NotMaster,
                str::stream() << "Not primary while removing from " << ns.ns(),
                !_request->shouldCallLogOp() ||
                repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(ns.db()));

        PlanExecutor* rawExec;
        if (_canonicalQuery.get()) {
            // This is the non-idhack branch.
            uassertStatusOK(getExecutorDelete(_request->getOpCtx(), collection,
                                              _canonicalQuery.release(), _request->isMulti(),
                                              _request->shouldCallLogOp(), &rawExec));
        }
        else {
            // This is the idhack branch.
            uassertStatusOK(getExecutorDelete(_request->getOpCtx(), collection, ns.ns(),
                                              _request->getQuery(), _request->isMulti(),
                                              _request->shouldCallLogOp(), &rawExec));
        }
        scoped_ptr<PlanExecutor> exec(rawExec);

        // Concurrently mutating state (by us) so we need to register 'exec'.
        const ScopedExecutorRegistration safety(exec.get());

        uassertStatusOK(exec->executePlan());

        // Extract the number of documents deleted from the DeleteStage stats.
        invariant(exec->getRootStage()->stageType() == STAGE_DELETE);
        DeleteStage* deleteStage = static_cast<DeleteStage*>(exec->getRootStage());
        const DeleteStats* deleteStats =
            static_cast<const DeleteStats*>(deleteStage->getSpecificStats());
        return deleteStats->docsDeleted;
    }

}  // namespace mongo
