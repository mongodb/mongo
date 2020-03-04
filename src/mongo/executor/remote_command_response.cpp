/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/executor/remote_command_response.h"

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/str.h"

namespace mongo {
namespace executor {

RemoteCommandResponseBase::RemoteCommandResponseBase(ErrorCodes::Error code, std::string reason)
    : status(code, reason){};

RemoteCommandResponseBase::RemoteCommandResponseBase(ErrorCodes::Error code,
                                                     std::string reason,
                                                     Milliseconds millis)
    : elapsedMillis(millis), status(code, reason) {}

RemoteCommandResponseBase::RemoteCommandResponseBase(Status s) : status(std::move(s)) {
    invariant(!isOK());
};

RemoteCommandResponseBase::RemoteCommandResponseBase(Status s, Milliseconds millis)
    : elapsedMillis(millis), status(std::move(s)) {
    invariant(!isOK());
};

RemoteCommandResponseBase::RemoteCommandResponseBase(BSONObj dataObj,
                                                     Milliseconds millis,
                                                     bool moreToCome)
    : data(std::move(dataObj)), elapsedMillis(millis), moreToCome(moreToCome) {
    // The buffer backing the default empty BSONObj has static duration so it is effectively
    // owned.
    invariant(data.isOwned() || data.objdata() == BSONObj().objdata());
};

// TODO(amidvidy): we currently discard output docs when we use this constructor. We should
// have RCR hold those too, but we need more machinery before that is possible.
RemoteCommandResponseBase::RemoteCommandResponseBase(const rpc::ReplyInterface& rpcReply,
                                                     Milliseconds millis,
                                                     bool moreToCome)
    : RemoteCommandResponseBase(rpcReply.getCommandReply(), std::move(millis), moreToCome) {}

bool RemoteCommandResponseBase::isOK() const {
    return status.isOK();
}

std::string RemoteCommandResponse::toString() const {
    return str::stream() << "RemoteResponse -- "
                         << " cmd:" << data.toString();
}

bool RemoteCommandResponse::operator==(const RemoteCommandResponse& rhs) const {
    if (this == &rhs) {
        return true;
    }
    SimpleBSONObjComparator bsonComparator;
    return bsonComparator.evaluate(data == rhs.data) && elapsedMillis == rhs.elapsedMillis;
}

bool RemoteCommandResponse::operator!=(const RemoteCommandResponse& rhs) const {
    return !(*this == rhs);
}

std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& response) {
    return os << response.toString();
}

RemoteCommandResponse::RemoteCommandResponse(const RemoteCommandOnAnyResponse& other)
    : RemoteCommandResponseBase(other) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp,
                                                       ErrorCodes::Error code,
                                                       std::string reason)
    : RemoteCommandResponseBase(code, std::move(reason)), target(std::move(hp)) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp,
                                                       ErrorCodes::Error code,
                                                       std::string reason,
                                                       Milliseconds millis)
    : RemoteCommandResponseBase(code, std::move(reason), millis), target(std::move(hp)) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp, Status s)
    : RemoteCommandResponseBase(std::move(s)), target(std::move(hp)) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp,
                                                       Status s,
                                                       Milliseconds millis)
    : RemoteCommandResponseBase(std::move(s), millis), target(std::move(hp)) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(HostAndPort hp,
                                                       BSONObj dataObj,
                                                       Milliseconds millis)
    : RemoteCommandResponseBase(std::move(dataObj), millis), target(std::move(hp)) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(HostAndPort hp,
                                                       const rpc::ReplyInterface& rpcReply,
                                                       Milliseconds millis)
    : RemoteCommandResponseBase(rpcReply, millis), target(std::move(hp)) {}

RemoteCommandOnAnyResponse::RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp,
                                                       const RemoteCommandResponse& other)
    : RemoteCommandResponseBase(other), target(std::move(hp)) {}

bool RemoteCommandOnAnyResponse::operator==(const RemoteCommandOnAnyResponse& rhs) const {
    if (this == &rhs) {
        return true;
    }
    SimpleBSONObjComparator bsonComparator;
    return bsonComparator.evaluate(data == rhs.data) && elapsedMillis == rhs.elapsedMillis &&
        target == rhs.target;
}

bool RemoteCommandOnAnyResponse::operator!=(const RemoteCommandOnAnyResponse& rhs) const {
    return !(*this == rhs);
}

std::string RemoteCommandOnAnyResponse::toString() const {
    return str::stream() << "RemoteOnAnyResponse -- "
                         << " cmd:" << data.toString() << " target: "
                         << (!target ? StringData("[none]") : StringData(target->toString()));
}

std::ostream& operator<<(std::ostream& os, const RemoteCommandOnAnyResponse& response) {
    return os << response.toString();
}

}  // namespace executor
}  // namespace mongo
