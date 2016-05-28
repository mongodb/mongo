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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/killcursors_response.h"

#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {

namespace {

const char kKilledField[] = "cursorsKilled";
const char kNotFoundField[] = "cursorsNotFound";
const char kAliveField[] = "cursorsAlive";
const char kUnknownField[] = "cursorsUnknown";

Status fillOutCursorArray(const BSONObj& cmdResponse,
                          StringData fieldName,
                          std::vector<CursorId>* cursorIds) {
    BSONElement elt = cmdResponse[fieldName];

    if (elt.type() != BSONType::Array) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Field '" << fieldName << "' must be of type array in: "
                              << cmdResponse};
    }

    for (BSONElement cursorElt : elt.Obj()) {
        if (cursorElt.type() != BSONType::NumberLong) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "Field '" << fieldName
                                  << "' contains an element that is not of type long: "
                                  << cursorElt};
        }
        cursorIds->push_back(cursorElt.numberLong());
    }

    return Status::OK();
}

void addCursorArrayToBSON(const std::vector<CursorId>& cursorIds,
                          StringData fieldName,
                          BSONObjBuilder* builder) {
    BSONArrayBuilder idsBuilder(builder->subarrayStart(fieldName));
    for (CursorId id : cursorIds) {
        idsBuilder.append(id);
    }
    idsBuilder.doneFast();
}

}  // namespace

KillCursorsResponse::KillCursorsResponse() {}

KillCursorsResponse::KillCursorsResponse(const std::vector<CursorId>& killed,
                                         const std::vector<CursorId>& notFound,
                                         const std::vector<CursorId>& alive,
                                         const std::vector<CursorId>& unknown)
    : cursorsKilled(killed),
      cursorsNotFound(notFound),
      cursorsAlive(alive),
      cursorsUnknown(unknown) {}

StatusWith<KillCursorsResponse> KillCursorsResponse::parseFromBSON(const BSONObj& cmdResponse) {
    Status cmdStatus = getStatusFromCommandResult(cmdResponse);
    if (!cmdStatus.isOK()) {
        return cmdStatus;
    }

    std::vector<CursorId> cursorsKilled;
    Status killedStatus = fillOutCursorArray(cmdResponse, kKilledField, &cursorsKilled);
    if (!killedStatus.isOK()) {
        return killedStatus;
    }

    std::vector<CursorId> cursorsNotFound;
    Status notFoundStatus = fillOutCursorArray(cmdResponse, kNotFoundField, &cursorsNotFound);
    if (!notFoundStatus.isOK()) {
        return notFoundStatus;
    }

    std::vector<CursorId> cursorsAlive;
    Status aliveStatus = fillOutCursorArray(cmdResponse, kAliveField, &cursorsAlive);
    if (!aliveStatus.isOK()) {
        return aliveStatus;
    }

    std::vector<CursorId> cursorsUnknown;
    Status unknownStatus = fillOutCursorArray(cmdResponse, kUnknownField, &cursorsUnknown);
    if (!unknownStatus.isOK()) {
        return unknownStatus;
    }

    return KillCursorsResponse(cursorsKilled, cursorsNotFound, cursorsAlive, cursorsUnknown);
}

BSONObj KillCursorsResponse::toBSON() const {
    BSONObjBuilder builder;
    addToBSON(&builder);
    builder.append("ok", 1.0);
    return builder.obj();
}

void KillCursorsResponse::addToBSON(BSONObjBuilder* builder) const {
    addCursorArrayToBSON(cursorsKilled, kKilledField, builder);
    addCursorArrayToBSON(cursorsNotFound, kNotFoundField, builder);
    addCursorArrayToBSON(cursorsAlive, kAliveField, builder);
    addCursorArrayToBSON(cursorsUnknown, kUnknownField, builder);
}

}  // namespace mongo
