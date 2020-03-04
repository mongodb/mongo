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

#pragma once

#include <boost/optional.hpp>
#include <iosfwd>
#include <memory>
#include <string>

#include "mongo/base/status.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/message.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace rpc {
class ReplyInterface;
}  // namespace rpc

namespace executor {


/**
 * Type of object describing the response of previously sent RemoteCommandRequest.
 */
struct RemoteCommandResponseBase {
    RemoteCommandResponseBase() = default;

    RemoteCommandResponseBase(ErrorCodes::Error code, std::string reason);

    RemoteCommandResponseBase(ErrorCodes::Error code, std::string reason, Milliseconds millis);

    RemoteCommandResponseBase(Status s);

    RemoteCommandResponseBase(Status s, Milliseconds millis);

    RemoteCommandResponseBase(BSONObj dataObj, Milliseconds millis, bool moreToCome = false);

    RemoteCommandResponseBase(const rpc::ReplyInterface& rpcReply,
                              Milliseconds millis,
                              bool moreToCome = false);

    bool isOK() const;

    BSONObj data;  // Always owned. May point into message.
    boost::optional<Milliseconds> elapsedMillis;
    Status status = Status::OK();
    bool moreToCome = false;  // Whether or not the moreToCome bit is set on an exhaust message.

protected:
    ~RemoteCommandResponseBase() = default;
};

struct RemoteCommandOnAnyResponse;

struct RemoteCommandResponse : RemoteCommandResponseBase {
    using RemoteCommandResponseBase::RemoteCommandResponseBase;

    RemoteCommandResponse(const RemoteCommandOnAnyResponse& other);

    std::string toString() const;

    bool operator==(const RemoteCommandResponse& rhs) const;
    bool operator!=(const RemoteCommandResponse& rhs) const;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& request);
};

/**
 * This type is a RemoteCommandResponse + the target that the origin request was actually run on.
 *
 * For the moment, it is only returned by scheduleRemoteCommandOnAny, and should be thought of as a
 * different return type for that rpc api, rather than a higher-information RemoteCommandResponse.
 */
struct RemoteCommandOnAnyResponse : RemoteCommandResponseBase {
    RemoteCommandOnAnyResponse() = default;

    RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp,
                               ErrorCodes::Error code,
                               std::string reason);

    RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp,
                               ErrorCodes::Error code,
                               std::string reason,
                               Milliseconds millis);

    RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp, Status s);

    RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp, Status s, Milliseconds millis);

    RemoteCommandOnAnyResponse(HostAndPort hp, BSONObj dataObj, Milliseconds millis);

    RemoteCommandOnAnyResponse(HostAndPort hp,
                               const rpc::ReplyInterface& rpcReply,
                               Milliseconds millis);

    RemoteCommandOnAnyResponse(boost::optional<HostAndPort> hp, const RemoteCommandResponse& other);

    std::string toString() const;

    bool operator==(const RemoteCommandOnAnyResponse& rhs) const;
    bool operator!=(const RemoteCommandOnAnyResponse& rhs) const;

    boost::optional<HostAndPort> target;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandOnAnyResponse& request);
};

}  // namespace executor
}  // namespace mongo
