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

#include "mongo/s/query/store_possible_cursor.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/s/cursors.h"
#include "mongo/s/strategy.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {
Status storePossibleCursorLegacy(const HostAndPort& server, const BSONObj& cmdResult) {
    if (cmdResult["ok"].trueValue() && cmdResult.hasField("cursor")) {
        BSONElement cursorIdElt = cmdResult.getFieldDotted("cursor.id");

        if (cursorIdElt.type() != mongo::NumberLong) {
            return Status(ErrorCodes::TypeMismatch,
                          str::stream() << "expected \"cursor.id\" field from shard "
                                        << "response to have NumberLong type, instead "
                                        << "got: " << typeName(cursorIdElt.type()));
        }

        const long long cursorId = cursorIdElt.Long();
        if (cursorId != 0) {
            BSONElement cursorNsElt = cmdResult.getFieldDotted("cursor.ns");
            if (cursorNsElt.type() != mongo::String) {
                return Status(ErrorCodes::TypeMismatch,
                              str::stream() << "expected \"cursor.ns\" field from "
                                            << "shard response to have String type, "
                                            << "instead got: " << typeName(cursorNsElt.type()));
            }

            const std::string cursorNs = cursorNsElt.String();
            cursorCache.storeRef(server.toString(), cursorId, cursorNs);
        }
    }

    return Status::OK();
}
}  // namespace

StatusWith<BSONObj> storePossibleCursor(const HostAndPort& server,
                                        const BSONObj& cmdResult,
                                        executor::TaskExecutor* executor,
                                        ClusterCursorManager* cursorManager) {
    if (!useClusterClientCursor) {
        Status status = storePossibleCursorLegacy(server, cmdResult);
        return (status.isOK() ? StatusWith<BSONObj>(cmdResult) : StatusWith<BSONObj>(status));
    }

    if (!cmdResult["ok"].trueValue() || !cmdResult.hasField("cursor")) {
        return cmdResult;
    }

    auto incomingCursorResponse = CursorResponse::parseFromBSON(cmdResult);
    if (!incomingCursorResponse.isOK()) {
        return incomingCursorResponse.getStatus();
    }

    if (incomingCursorResponse.getValue().getCursorId() == CursorId(0)) {
        return cmdResult;
    }

    ClusterClientCursorParams params(incomingCursorResponse.getValue().getNSS());
    params.remotes.emplace_back(server, incomingCursorResponse.getValue().getCursorId());

    auto ccc = stdx::make_unique<ClusterClientCursorImpl>(executor, std::move(params));
    auto pinnedCursor =
        cursorManager->registerCursor(std::move(ccc),
                                      incomingCursorResponse.getValue().getNSS(),
                                      ClusterCursorManager::CursorType::NamespaceNotSharded,
                                      ClusterCursorManager::CursorLifetime::Mortal);
    CursorId clusterCursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);

    CursorResponse outgoingCursorResponse(incomingCursorResponse.getValue().getNSS(),
                                          clusterCursorId,
                                          incomingCursorResponse.getValue().getBatch());
    return outgoingCursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
}

}  // namespace mongo
