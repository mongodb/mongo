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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/rpc/message.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <iosfwd>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace rpc {
class ReplyInterface;
}  // namespace rpc

namespace executor {


/**
 * Type of object describing the response of previously sent RemoteCommandRequest.
 *
 * The target member may be used by callers to associate a HostAndPort with the remote or
 * local error that the RemoteCommandResponse holds. The "status" member will be populated
 * with possible local errors, while the "data" member may hold any remote errors.
 *
 */
struct RemoteCommandResponse {
    RemoteCommandResponse() = default;

    RemoteCommandResponse(HostAndPort hp, Status s);

    RemoteCommandResponse(HostAndPort hp, Status s, Microseconds elapsed);

    RemoteCommandResponse(HostAndPort hp,
                          BSONObj dataObj,
                          Microseconds elapsed,
                          bool moreToCome = false);

    std::string toString() const;

    bool operator==(const RemoteCommandResponse& rhs) const;
    bool operator!=(const RemoteCommandResponse& rhs) const;

    bool isOK() const;

    static RemoteCommandResponse make_forTest(Status s);
    static RemoteCommandResponse make_forTest(Status s, Microseconds elapsed);
    static RemoteCommandResponse make_forTest(BSONObj dataObj,
                                              Microseconds elapsed,
                                              bool moreToCome = false);

    BSONObj data;  // Always owned. May point into message.
    boost::optional<Microseconds> elapsed;
    Status status = Status::OK();
    bool moreToCome = false;  // Whether or not the moreToCome bit is set on an exhaust message.
    HostAndPort target;

    friend std::ostream& operator<<(std::ostream& os, const RemoteCommandResponse& request);

private:
    RemoteCommandResponse(Status s);
    RemoteCommandResponse(Status s, Microseconds elapsed);
    RemoteCommandResponse(BSONObj dataObj, Microseconds elapsed, bool moreToCome);
};
}  // namespace executor
}  // namespace mongo
