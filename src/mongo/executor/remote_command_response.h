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

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <iosfwd>
#include <memory>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/message.h"
#include "mongo/util/duration.h"
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

    RemoteCommandResponseBase(ErrorCodes::Error code, std::string reason, Microseconds elapsed);

    RemoteCommandResponseBase(Status s);

    RemoteCommandResponseBase(Status s, Microseconds elapsed);

    RemoteCommandResponseBase(BSONObj dataObj, Microseconds elapsed, bool moreToCome = false);

    RemoteCommandResponseBase(const rpc::ReplyInterface& rpcReply,
                              Microseconds elapsed,
                              bool moreToCome = false);

    bool isOK() const;

    BSONObj data;  // Always owned. May point into message.
    boost::optional<Microseconds> elapsed;
    Status status = Status::OK();
    bool moreToCome = false;  // Whether or not the moreToCome bit is set on an exhaust message.

protected:
    ~RemoteCommandResponseBase() = default;
};

/**
 * This type is a RemoteCommandResponseBase + the target that the origin request was actually run
 * on.
 *
 * The target member may be used by callers to associate a HostAndPort with the remote or
 * local error that the RemoteCommandResponse holds. The "status" member will be populated
 * with possible local errors, while the "data" member may hold any remote errors. For local
 * errors that are not caused by the remote (for example, local shutdown), the target member will
 * not be filled.
 *
 * For local errors, the response is associated (by the network interface) with a remote
 * HostAndPort for these cases:
 *   1. When acquiring a connection to the remote from the pool.
 *   2. When using the connection to the remote.
 *   3. Enforcing a timeout (propagated or internal) while using the connection to the remote.
 */
struct RemoteCommandResponse : RemoteCommandResponseBase {
    using RemoteCommandResponseBase::RemoteCommandResponseBase;

    RemoteCommandResponse(boost::optional<HostAndPort> hp,
                          ErrorCodes::Error code,
                          std::string reason);

    RemoteCommandResponse(boost::optional<HostAndPort> hp,
                          ErrorCodes::Error code,
                          std::string reason,
                          Microseconds elapsed);

    RemoteCommandResponse(boost::optional<HostAndPort> hp, Status s);

    RemoteCommandResponse(boost::optional<HostAndPort> hp, Status s, Microseconds elapsed);

    RemoteCommandResponse(HostAndPort hp, BSONObj dataObj, Microseconds elapsed);

    RemoteCommandResponse(HostAndPort hp,
                          const rpc::ReplyInterface& rpcReply,
                          Microseconds elapsed);

    std::string toString() const;

    bool operator==(const RemoteCommandResponse& rhs) const;
    bool operator!=(const RemoteCommandResponse& rhs) const;

    boost::optional<HostAndPort> target;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& request);
};

}  // namespace executor
}  // namespace mongo
