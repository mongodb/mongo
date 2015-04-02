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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/network_interface_impl_downconvert_find_getmore.h"

#include <memory>

#include "mongo/client/dbclientinterface.h"
#include "mongo/db/commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/getmore_request.h"

namespace mongo {
namespace repl {

    Status runDownconvertedFindCommand(DBClientConnection* conn,
                                       const std::string& dbname,
                                       const BSONObj& cmdObj,
                                       BSONObj* output) {
        NamespaceString nss(dbname, cmdObj.firstElement().String());
        const std::string& ns = nss.ns();
        std::unique_ptr<LiteParsedQuery> lpq;
        {
            LiteParsedQuery* lpqRaw;
            // It is a little heavy handed to use LiteParsedQuery to convert the command
            // object to query() arguments but we get validation and consistent behavior
            // with the find command implementation on the remote server.
            Status status = LiteParsedQuery::make(ns, cmdObj, false, &lpqRaw);
            if (!status.isOK()) {
                return status;
            }
            lpq.reset(lpqRaw);
        }

        Query query(lpq->getFilter());
        if (!lpq->getSort().isEmpty()) { query.sort(lpq->getSort()); }
        if (!lpq->getHint().isEmpty()) { query.hint(lpq->getHint()); }
        if (!lpq->getMin().isEmpty()) { query.minKey(lpq->getMin()); }
        if (!lpq->getMax().isEmpty()) { query.minKey(lpq->getMax()); }
        if (lpq->isExplain()) { query.explain(); }
        if (lpq->isSnapshot()) { query.snapshot(); }
        int nToReturn = lpq->wantMore() ? lpq->getLimit() : -lpq->getLimit();
        int nToSkip = lpq->getSkip();
        const BSONObj* fieldsToReturn = &lpq->getProj();
        int queryOptions = lpq->getOptions();
        int batchSize = lpq->getBatchSize();

        std::unique_ptr<DBClientCursor> cursor =
            conn->query(ns, query, nToReturn, nToSkip, fieldsToReturn, queryOptions, batchSize);
        cursor->decouple();

        BSONArrayBuilder batch;
        while (cursor->moreInCurrentBatch()) {
            batch.append(cursor->next());
        }
        BSONObjBuilder result;
        Command::appendCursorResponseObject(cursor->getCursorId(), ns, batch.arr(), &result);
        Command::appendCommandStatus(result, Status::OK());
        *output = result.obj();
        return Status::OK();
    }

    Status runDownconvertedGetMoreCommand(DBClientConnection* conn,
                                          const std::string& dbname,
                                          const BSONObj& cmdObj,
                                          BSONObj* output) {
        StatusWith<GetMoreRequest> parseResult = GetMoreRequest::parseFromBSON(dbname, cmdObj);
        if (!parseResult.isOK()) {
            return parseResult.getStatus();
        }
        const GetMoreRequest& req = parseResult.getValue();
        const std::string& ns = req.nss.ns();

        std::unique_ptr<DBClientCursor> cursor = conn->getMore(ns, req.cursorid, req.batchSize);
        cursor->decouple();

        BSONArrayBuilder batch;
        while (cursor->moreInCurrentBatch()) {
            batch.append(cursor->next());
        }
        BSONObjBuilder result;
        Command::appendGetMoreResponseObject(cursor->getCursorId(), ns, batch.arr(), &result);
        Command::appendCommandStatus(result, Status::OK());
        *output = result.obj();
        return Status::OK();
    }

}  // namespace repl
} // namespace mongo
