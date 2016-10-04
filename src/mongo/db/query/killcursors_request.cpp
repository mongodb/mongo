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

#include "mongo/db/query/killcursors_request.h"

namespace mongo {

namespace {

const char kCmdName[] = "killCursors";
const char kCursorsField[] = "cursors";

}  // namespace

KillCursorsRequest::KillCursorsRequest(const NamespaceString& nsString,
                                       const std::vector<CursorId>& ids)
    : nss(nsString), cursorIds(ids) {}

StatusWith<KillCursorsRequest> KillCursorsRequest::parseFromBSON(const std::string& dbname,
                                                                 const BSONObj& cmdObj) {
    if (!str::equals(cmdObj.firstElement().fieldName(), kCmdName)) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "First field name must be '" << kCmdName << "' in: " << cmdObj};
    }

    if (cmdObj.firstElement().type() != BSONType::String) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "First parameter must be a string in: " << cmdObj};
    }

    std::string collName = cmdObj.firstElement().String();
    const NamespaceString nss(dbname, collName);
    if (!nss.isValid()) {
        return {ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid collection name: " << nss.ns()};
    }

    if (cmdObj[kCursorsField].type() != BSONType::Array) {
        return {ErrorCodes::FailedToParse,
                str::stream() << "Field '" << kCursorsField << "' must be of type array in: "
                              << cmdObj};
    }

    std::vector<CursorId> cursorIds;
    for (BSONElement cursorEl : cmdObj[kCursorsField].Obj()) {
        if (cursorEl.type() != BSONType::NumberLong) {
            return {ErrorCodes::FailedToParse,
                    str::stream() << "Field '" << kCursorsField
                                  << "' contains an element that is not of type long: "
                                  << cursorEl};
        }
        cursorIds.push_back(cursorEl.numberLong());
    }

    if (cursorIds.empty()) {
        return {ErrorCodes::BadValue,
                str::stream() << "Must specify at least one cursor id in: " << cmdObj};
    }

    return KillCursorsRequest(nss, cursorIds);
}

BSONObj KillCursorsRequest::toBSON() const {
    BSONObjBuilder builder;

    builder.append(kCmdName, nss.coll());
    BSONArrayBuilder idsBuilder(builder.subarrayStart(kCursorsField));
    for (CursorId id : cursorIds) {
        idsBuilder.append(id);
    }
    idsBuilder.doneFast();

    return builder.obj();
}

}  // namespace mongo
