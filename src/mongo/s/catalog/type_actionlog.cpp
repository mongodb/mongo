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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/catalog/type_actionlog.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const std::string ActionLogType::ConfigNS = "config.actionlog";

const BSONField<std::string> ActionLogType::server("server");
const BSONField<std::string> ActionLogType::what("what");
const BSONField<Date_t> ActionLogType::time("time");
const BSONField<BSONObj> ActionLogType::details("details");

StatusWith<ActionLogType> ActionLogType::fromBSON(const BSONObj& source) {
    ActionLogType actionLog;

    {
        std::string actionLogServer;
        Status status = bsonExtractStringField(source, server.name(), &actionLogServer);
        if (!status.isOK())
            return status;
        actionLog._server = actionLogServer;
    }

    {
        BSONElement actionLogTimeElem;
        Status status = bsonExtractTypedField(source, time.name(), Date, &actionLogTimeElem);
        if (!status.isOK())
            return status;
        actionLog._time = actionLogTimeElem.date();
    }

    {
        std::string actionLogWhat;
        Status status = bsonExtractStringField(source, what.name(), &actionLogWhat);
        if (!status.isOK())
            return status;
        actionLog._what = actionLogWhat;
    }

    {
        BSONElement actionLogDetailsElem;
        Status status =
            bsonExtractTypedField(source, details.name(), Object, &actionLogDetailsElem);
        if (!status.isOK())
            return status;
        actionLog._details = actionLogDetailsElem.Obj().getOwned();
    }

    return actionLog;
}

Status ActionLogType::validate() const {
    if (!_server.is_initialized() || _server->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << server.name() << " field"};
    }

    if (!_what.is_initialized() || _what->empty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << what.name() << " field"};
    }

    if (!_time.is_initialized()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << time.name() << " field"};
    }

    if (!_details.is_initialized() || _details->isEmpty()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "missing " << details.name() << " field"};
    }

    return Status::OK();
}

BSONObj ActionLogType::toBSON() const {
    BSONObjBuilder builder;

    if (_server)
        builder.append(server.name(), getServer());
    if (_what)
        builder.append(what.name(), getWhat());
    if (_time)
        builder.append(time.name(), getTime());
    if (_details)
        builder.append(details.name(), _details.get());

    return builder.obj();
}

std::string ActionLogType::toString() const {
    return toBSON().toString();
}

void ActionLogType::setServer(const std::string& server) {
    invariant(!server.empty());
    _server = server;
}

void ActionLogType::setWhat(const std::string& what) {
    invariant(!what.empty());
    _what = what;
}

void ActionLogType::setTime(const Date_t& time) {
    _time = time;
}

void ActionLogType::setDetails(const boost::optional<std::string>& errMsg,
                               int executionTime,
                               int candidateChunks,
                               int chunksMoved) {
    BSONObjBuilder builder;
    builder.append("executionTimeMillis", executionTime);
    builder.append("errorOccured", errMsg.is_initialized());

    if (errMsg) {
        builder.append("errmsg", errMsg.get());
    } else {
        builder.append("candidateChunks", candidateChunks);
        builder.append("chunksMoved", chunksMoved);
    }

    _details = builder.obj();
}

}  // namespace mongo
